#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Rust twin of `benchmarks/cpp/sync_bench.cpp`, same measurements and labels.
//!
//! The first three C++ loops are optimised away by the compiler (sub-nanosecond
//! "lock/unlock"), so they are not a baseline; here `black_box` keeps the work.
//! `TaskGroup spawn + wait` maps to `scope` with one child, the nearest match.
//!
//! Usage: `cargo bench --bench sync -- [--scale N]` (N divides every iteration count).

use std::cell::Cell;
use std::hint::black_box;
use std::rc::Rc;
use std::time::Instant;

use tinyio::{Cx, Io, Latch, Mutex, Options, Semaphore, scope};

fn arg(name: &str, default: u64) -> u64 {
    let args: Vec<String> = std::env::args().collect();
    args.iter()
        .position(|a| a == name)
        .and_then(|i| args.get(i + 1))
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

fn report(label: &str, start: Instant, ops: u64) {
    println!(
        "{label}: {} ns/op",
        start.elapsed().as_nanos() as f64 / ops as f64
    );
}

fn mutex_uncontended(cx: &Cx, n: u64) {
    let m = Mutex::new(cx, 0u8);
    let m = black_box(&m);
    let start = Instant::now();
    for _ in 0..n {
        drop(black_box(m.lock(cx)));
    }
    report("Mutex uncontended lock/unlock", start, n);
}

fn semaphore_uncontended(cx: &Cx, n: u64) {
    let s = Semaphore::new(cx, 1);
    let s = black_box(&s);
    let start = Instant::now();
    for _ in 0..n {
        s.acquire(cx);
        s.release();
    }
    report("Semaphore uncontended acquire/release", start, n);
}

fn latch_create_countdown(cx: &Cx, n: u64) {
    let start = Instant::now();
    for _ in 0..n {
        let l = black_box(Latch::new(cx, 1));
        l.count_down();
    }
    report("Latch create + countdown", start, n);
}

fn scope_spawn_wait(cx: &Cx, n: u64) {
    let start = Instant::now();
    for _ in 0..n {
        scope(cx, |s| {
            s.spawn(|_| {}).unwrap();
        });
    }
    report("TaskGroup spawn + wait", start, n);
}

fn mutex_contended(cx: &Cx, n: u64) {
    let m = Mutex::new(cx, 0u8);
    let start = Instant::now();
    scope(cx, |s| {
        s.spawn(|c| {
            for _ in 0..n {
                drop(m.lock(c));
                c.yield_now().unwrap();
            }
        })
        .unwrap();
        for _ in 0..n {
            drop(m.lock(cx));
            cx.yield_now().unwrap();
        }
    });
    report("Mutex contended lock/unlock (+yields)", start, n * 2);
}

fn main() {
    let scale = arg("--scale", 1).max(1);
    let n = 10_000_000 / scale;
    let mut io = Io::new(Options::default()).unwrap();
    let done = Rc::new(Cell::new(false));
    let d = done.clone();
    io.spawn(move |cx| {
        println!("--- Fiber Sync Primitives Benchmark ---");
        mutex_uncontended(cx, n);
        semaphore_uncontended(cx, n);
        latch_create_countdown(cx, n);
        scope_spawn_wait(cx, n / 10);
        mutex_contended(cx, n / 10);
        d.set(true);
    })
    .unwrap();
    io.run_until_done();
    assert!(done.get());
}
