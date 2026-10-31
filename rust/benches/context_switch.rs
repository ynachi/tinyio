#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Rust twin of `benchmarks/cpp/context_switch_bench.cpp`: one fiber yields
//! in a loop. Each yield is two context switches (fiber -> scheduler -> fiber).
//!
//! Usage: `cargo bench --bench context_switch -- [--iters N] [--rounds R]`
//! Prints one `ns_per_switch=<f>` line per round, then the median.

use std::cell::Cell;
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
    let mut io = Io::new(Options::default()).expect("Io::new");
    let elapsed = Rc::new(Cell::new(0u128));
    let out = elapsed.clone();
    io.spawn(move |cx| {
        let start = Instant::now();
        for _ in 0..iters {
            if cx.yield_now().is_err() {
                eprintln!("yield failed");
                break;
            }
        }
        out.set(start.elapsed().as_nanos());
    })
    .expect("spawn");
    io.run_until_done();
    elapsed.get() as f64 / (iters * 2) as f64
}

fn main() {
    let iters = arg("--iters", 10_000_000);
    let rounds = arg("--rounds", 5);
    let mut results: Vec<f64> = (0..rounds).map(|_| one_round(iters)).collect();
    for r in &results {
        println!("ns_per_switch={r:.3}");
    }
    results.sort_by(|a, b| a.total_cmp(b));
    println!("median ns/switch: {:.3}", results[results.len() / 2]);
}
