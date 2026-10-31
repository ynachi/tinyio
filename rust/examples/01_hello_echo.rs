#![forbid(unsafe_code)]
//! A concurrent echo server on one thread. The code reads like blocking code,
//! but every `read`, `write` and `accept` parks only its own fiber.
//!
//! Shows: the accept timeout (the server ends after `--accept-timeout-secs`
//! with no new client) and error handling with `?` and `io::Result`.
//!
//!     cargo run --example 01_hello_echo -- --port=8080

mod common;

use std::io;
use std::net::SocketAddr;
use std::time::Duration;

use common::flag;
use tinyio::net::{TcpListener, TcpStream};
use tinyio::{Cx, Io, Options};

fn handle_client(cx: &Cx, client: TcpStream) -> io::Result<()> {
    let mut buf = [0u8; 4096];
    loop {
        let n = cx.read(&client, &mut buf, None)?;
        if n == 0 {
            return Ok(()); // the client closed
        }
        let mut sent = 0;
        while sent < n {
            sent += cx.write(&client, &buf[sent..n], None)?;
        }
    }
}

fn main() -> io::Result<()> {
    let ip: String = flag("ip", "0.0.0.0".to_string());
    let port: u16 = flag("port", 8080);
    let accept_timeout = Duration::from_secs(flag("accept-timeout-secs", 30));
    let addr: SocketAddr = format!("{ip}:{port}")
        .parse()
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;

    let mut io = Io::new(Options::default())?;
    let listener = TcpListener::bind(addr)?;
    println!("listening on {}", listener.local_addr()?);

    io.spawn(move |cx| {
        loop {
            // No client within the timeout: the server shuts itself down.
            let fd = match cx.timeout(accept_timeout).accept(&listener) {
                Ok(fd) => fd,
                Err(e) => return eprintln!("accept: {e}"),
            };
            let spawned = cx.spawn(move |cx| {
                if let Err(e) = handle_client(cx, TcpStream::from(fd)) {
                    eprintln!("client: {e}");
                }
            });
            if let Err(e) = spawned {
                eprintln!("spawn: {e}");
            }
        }
    })?;
    io.run_until_done();
    Ok(())
}
