#![forbid(unsafe_code)]
//! A TCP health checker: probe a list of hosts concurrently and report UP or
//! DOWN with the connect latency.
//!
//! Shows: `scope` (fan-out, then fan-in, with children that borrow the
//! results), `Semaphore` as a throttle, and connect timeouts.
//!
//!     cargo run --example 02_fanout -- --port=443 example.com rust-lang.org

mod common;

use std::cell::RefCell;
use std::io;
use std::net::{SocketAddr, ToSocketAddrs};
use std::time::{Duration, Instant};

use common::{flag, positional};
use tinyio::net::TcpStream;
use tinyio::{Io, Options, Semaphore, scope};

fn main() -> io::Result<()> {
    let port: u16 = flag("port", 443);
    let hosts = positional();
    if hosts.is_empty() {
        eprintln!("usage: 02_fanout [--port=443] host1 host2 ...");
        std::process::exit(1);
    }

    // DNS blocks, so resolve before the loop runs.
    let mut targets: Vec<(String, SocketAddr)> = Vec::new();
    for host in hosts {
        match (host.as_str(), port)
            .to_socket_addrs()
            .map(|mut a| a.next())
        {
            Ok(Some(addr)) => targets.push((host, addr)),
            _ => eprintln!("{host:<24} resolve failed"),
        }
    }
    if targets.is_empty() {
        eprintln!("no valid targets to probe");
        std::process::exit(1);
    }

    let mut io = Io::new(Options::default())?;
    let gate = Semaphore::new(&io, 64); // at most 64 probes in flight
    io.spawn(move |cx| {
        let results: RefCell<Vec<Option<io::Result<f64>>>> =
            RefCell::new((0..targets.len()).map(|_| None).collect());
        scope(cx, |s| {
            for (i, (_, addr)) in targets.iter().enumerate() {
                let (gate, results) = (&gate, &results);
                let spawned = s.spawn(move |cx| {
                    let started = Instant::now();
                    // The permit goes back when it drops, however the probe ends.
                    let outcome = gate.acquire(cx).and_then(|_permit| {
                        TcpStream::connect_timeout(cx, *addr, Duration::from_secs(2))
                    });
                    let ms = started.elapsed().as_secs_f64() * 1000.0;
                    results.borrow_mut()[i] = Some(outcome.map(|_| ms));
                });
                if let Err(e) = spawned {
                    results.borrow_mut()[i] = Some(Err(e));
                }
            }
        }); // every probe has finished here
        for ((host, _), result) in targets.iter().zip(results.borrow().iter()) {
            match result {
                Some(Ok(ms)) => println!("{host:<24} UP    {ms:>6.1} ms"),
                Some(Err(e)) if e.kind() == io::ErrorKind::TimedOut => {
                    println!("{host:<24} DOWN  (timeout)")
                }
                Some(Err(e)) if e.kind() == io::ErrorKind::ConnectionRefused => {
                    println!("{host:<24} DOWN  (refused)")
                }
                Some(Err(e)) => println!("{host:<24} DOWN  ({e})"),
                None => println!("{host:<24} not probed"),
            }
        }
    })?;
    io.run_until_done();
    Ok(())
}
