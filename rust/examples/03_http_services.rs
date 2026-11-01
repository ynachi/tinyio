#![forbid(unsafe_code)]
//! A multi-threaded HTTP/1.1 keep-alive server: one `Io` per thread, every
//! thread accepting on the same port (`SO_REUSEPORT`).
//!
//! Shows: thread-per-core, a fiber per connection, and a clean shutdown
//! through `Remote::stop` (`--seconds=N`, or Ctrl-C to kill it).
//!
//!     cargo run --example 03_http_services -- --port=8080 --workers=4
//!     curl -v http://127.0.0.1:8080/

mod common;

use std::io;
use std::net::SocketAddr;

use common::{flag, run_workers};
use tinyio::net::{TcpListener, TcpStream};
use tinyio::{Cx, is_cancelled};

const RESPONSE: &[u8] = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\
Content-Length: 16\r\nConnection: keep-alive\r\n\r\nHello, io_uring!";

fn handle_client(cx: &Cx, client: TcpStream) -> io::Result<()> {
    let mut buf = [0u8; 1024];
    loop {
        if cx.read(&client, &mut buf, None)? == 0 {
            return Ok(());
        }
        let mut sent = 0;
        while sent < RESPONSE.len() {
            sent += cx.send(&client, &RESPONSE[sent..])?;
        }
    }
}

fn main() {
    let port: u16 = flag("port", 8080);
    let workers: usize = flag("workers", 4);
    let seconds: u64 = flag("seconds", 0);
    println!("http server on 0.0.0.0:{port} with {workers} workers");

    run_workers(workers, seconds, move |io, worker| {
        let addr = SocketAddr::from(([0, 0, 0, 0], port));
        let listener = match TcpListener::bind_reuse_port(addr) {
            Ok(l) => l,
            Err(e) => return eprintln!("worker {worker}: bind: {e}"),
        };
        let _ = io.spawn(move |cx| {
            loop {
                let fd = match cx.accept(&listener) {
                    Ok(fd) => fd,
                    Err(e) if is_cancelled(&e) => return, // shutting down
                    Err(e) => {
                        eprintln!("worker {worker}: accept: {e}");
                        let _ = cx.sleep(std::time::Duration::from_millis(10)); // e.g. EMFILE: do not spin
                        continue;
                    }
                };
                let _ = cx.spawn(move |cx| {
                    if let Err(e) = handle_client(cx, TcpStream::from(fd))
                        && !is_cancelled(&e)
                    {
                        eprintln!("worker {worker}: client: {e}");
                    }
                });
            }
        });
    });
}
