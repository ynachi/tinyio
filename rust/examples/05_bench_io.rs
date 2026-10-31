#![forbid(unsafe_code)]
//! Disk and network micro-benchmarks, one mode per run:
//!
//!     05_bench_io write  [--file=bench.dat] [--mb=1024]    sequential 64 KiB writes
//!     05_bench_io read   [--file=bench.dat] [--mb=1024] [--reads=100000]   random 4 KiB reads
//!     05_bench_io server [--port=8082] [--workers=4] [--seconds=0]   an HTTP hello server
//!
//! For numbers worth quoting: a release build, a freshly written data file
//! (a stale file on an SSD with an SLC cache reads about twice as slow), and a
//! quiet machine.

mod common;

use std::io;
use std::net::SocketAddr;
use std::time::Instant;

use common::{flag, positional, run_workers};
use tinyio::net::{TcpListener, TcpStream};
use tinyio::{Cx, Io, OpenOptions, Options};

fn do_write(cx: &Cx, file: &str, bytes: u64) -> io::Result<()> {
    let fd = cx.open(
        file,
        OpenOptions::new().write(true).create(true).truncate(true),
    )?;
    let block = vec![b'A'; 64 * 1024];
    let start = Instant::now();
    let mut offset = 0u64;
    while offset < bytes {
        let mut done = 0;
        while done < block.len() {
            done += cx.write(&fd, &block[done..], Some(offset + done as u64))?;
        }
        offset += block.len() as u64;
    }
    cx.close(fd)?;
    println!(
        "Sequential write ({} MiB): {} ms",
        bytes >> 20,
        start.elapsed().as_millis()
    );
    Ok(())
}

fn do_read(cx: &Cx, file: &str, bytes: u64, reads: u64) -> io::Result<()> {
    let fd = cx.open(file, OpenOptions::new().read(true))?;
    let mut block = vec![0u8; 4096];
    let mut state = 42u64; // xorshift64
    let start = Instant::now();
    for _ in 0..reads {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        let offset = state % (bytes - 4096);
        cx.read(&fd, &mut block, Some(offset))?;
    }
    cx.close(fd)?;
    println!(
        "Random read ({reads} x 4 KiB): {} ms",
        start.elapsed().as_millis()
    );
    Ok(())
}

const RESPONSE: &[u8] =
    b"HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: keep-alive\r\n\r\nHello, World!";

fn serve(cx: &Cx, client: TcpStream) -> io::Result<()> {
    let mut buf = [0u8; 4096];
    while cx.read(&client, &mut buf, None)? != 0 {
        cx.write(&client, RESPONSE, None)?;
    }
    Ok(())
}

fn main() -> io::Result<()> {
    let mode = positional()
        .into_iter()
        .next()
        .unwrap_or_else(|| "write".to_string());
    match mode.as_str() {
        "write" | "read" => {
            let file: String = flag("file", "bench.dat".to_string());
            let bytes = flag::<u64>("mb", 1024) << 20;
            let reads: u64 = flag("reads", 100_000);
            let mut io = Io::new(Options::default())?;
            io.spawn(move |cx| {
                let r = if mode == "write" {
                    do_write(cx, &file, bytes)
                } else {
                    do_read(cx, &file, bytes, reads)
                };
                if let Err(e) = r {
                    eprintln!("{mode}: {e}");
                }
            })?;
            io.run_until_done();
        }
        "server" => {
            let port: u16 = flag("port", 8082);
            run_workers(flag("workers", 4), flag("seconds", 0), move |io, worker| {
                let Ok(listener) = TcpListener::bind(SocketAddr::from(([0, 0, 0, 0], port))) else {
                    return eprintln!("worker {worker}: bind failed");
                };
                let _ = io.spawn(move |cx| {
                    while let Ok(fd) = cx.accept(&listener) {
                        let _ = cx.spawn(|cx| drop(serve(cx, TcpStream::from(fd))));
                    }
                });
            });
        }
        other => {
            eprintln!("unknown mode {other:?}; use write, read or server");
            std::process::exit(2);
        }
    }
    Ok(())
}
