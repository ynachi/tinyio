#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Rust twin of `benchmarks/cpp/pingpong_bench.cpp`: two fibers ping-pong one
//! byte over a socketpair. Each round trip is two writes and two reads, so it
//! measures the whole syscall path (SQE, suspend, submit, reap, resume).
//!
//! Usage: `cargo bench --bench pingpong -- [--iters N] [--rounds R]`
//! Prints one `ns_per_op=<f>` line (ns per round trip) per round, then the median.

use std::cell::Cell;
use std::os::unix::net::UnixStream;
use std::rc::Rc;
use std::time::Instant;

use tinyio::{Io, Options};

fn arg(name: &str, default: u64) -> u64 {
    let args: Vec<String> = std::env::args().collect();
    args.iter()
        .position(|a| a == name)
        .and_then(|i| args.get(i + 1))
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

fn one_round(iters: u64) -> f64 {
    let (a, b) = UnixStream::pair().unwrap();
    let mut io = Io::new(Options::default()).unwrap();
    let elapsed = Rc::new(Cell::new(0u128));
    let out = elapsed.clone();
    io.spawn(move |cx| {
        let mut byte = [0u8; 1];
        let start = Instant::now();
        for _ in 0..iters {
            cx.write(&a, &byte, None).unwrap();
            cx.read(&a, &mut byte, None).unwrap();
        }
        out.set(start.elapsed().as_nanos());
    })
    .unwrap();
    io.spawn(move |cx| {
        let mut byte = [0u8; 1];
        for _ in 0..iters {
            cx.read(&b, &mut byte, None).unwrap();
            cx.write(&b, &byte, None).unwrap();
        }
    })
    .unwrap();
    io.run_until_done();
    elapsed.get() as f64 / iters as f64
}

fn main() {
    let iters = arg("--iters", 200_000);
    let rounds = arg("--rounds", 5);
    let mut results: Vec<f64> = (0..rounds).map(|_| one_round(iters)).collect();
    for r in &results {
        println!("ns_per_op={r:.3}");
    }
    results.sort_by(|a, b| a.total_cmp(b));
    println!("median ns/roundtrip: {:.3}", results[results.len() / 2]);
}
