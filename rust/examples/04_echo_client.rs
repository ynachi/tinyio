#![forbid(unsafe_code)]
//! A multi-threaded echo client: several threads, each with an `Io` running
//! many client fibers, every one sending messages and checking the echo.
//! Exits non-zero if any client fails.
//!
//!     cargo run --example 04_echo_client -- --port=8080 --threads=4

mod common;

use std::io;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;

use common::flag;
use tinyio::net::TcpStream;
use tinyio::{Cx, Io, Options};

static SUCCESS: AtomicU64 = AtomicU64::new(0);
static ERRORS: AtomicU64 = AtomicU64::new(0);

fn run_client(cx: &Cx, addr: SocketAddr, name: &str, messages: u64) -> io::Result<()> {
    let conn = TcpStream::connect(cx, addr)?;
    let msg = format!("Hello from {name}");
    let mut buf = vec![0u8; msg.len()];
    for _ in 0..messages {
        let mut sent = 0;
        while sent < msg.len() {
            sent += cx.send(&conn, &msg.as_bytes()[sent..])?;
        }
        let mut got = 0;
        while got < buf.len() {
            let n = cx.timeout(std::time::Duration::from_secs(10)).read(
                &conn,
                &mut buf[got..],
                None,
            )?;
            if n == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "server closed",
                ));
            }
            got += n;
        }
        if buf != msg.as_bytes() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "corrupted echo"));
        }
    }
    Ok(())
}

fn worker(thread_id: u64, addr: SocketAddr, clients: u64, messages: u64) -> io::Result<()> {
    let mut io = Io::new(Options::default())?;
    for c in 0..clients {
        io.spawn(move |cx| {
            match run_client(
                cx,
                addr,
                &format!("thread {thread_id} client {c}"),
                messages,
            ) {
                Ok(()) => drop(SUCCESS.fetch_add(1, Ordering::Relaxed)),
                Err(e) => {
                    ERRORS.fetch_add(1, Ordering::Relaxed);
                    eprintln!("client {thread_id}-{c}: {e}");
                }
            }
        })?;
    }
    io.run_until_done();
    Ok(())
}

fn main() {
    let ip: String = flag("ip", "127.0.0.1".to_string());
    let port: u16 = flag("port", 8080);
    let threads: u64 = flag("threads", 4);
    let clients: u64 = flag("clients-per-thread", 10);
    let messages: u64 = flag("messages", 100);
    let Ok(addr) = format!("{ip}:{port}").parse::<SocketAddr>() else {
        eprintln!("bad --ip/--port");
        std::process::exit(2);
    };

    let handles: Vec<_> = (0..threads)
        .map(|t| thread::spawn(move || worker(t, addr, clients, messages)))
        .collect();
    for h in handles {
        if let Ok(Err(e)) = h.join() {
            ERRORS.fetch_add(1, Ordering::Relaxed);
            eprintln!("worker: {e}");
        }
    }
    println!("total success: {}", SUCCESS.load(Ordering::Relaxed));
    println!("total errors: {}", ERRORS.load(Ordering::Relaxed));
    std::process::exit(i32::from(ERRORS.load(Ordering::Relaxed) != 0));
}
