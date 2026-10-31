#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Rust twin of `benchmarks/cpp/post_bench.cpp`: producer threads post no-op
//! closures to an owner running `run()`; the last one stops it. Each row is
//! wall time per post, with an 8-byte capture (one pointer) in both languages.
//!
//! Usage: `cargo bench --bench post`

use std::sync::atomic::{AtomicUsize, Ordering};
use std::thread;
use std::time::Instant;

use tinyio::{Io, Options, Remote};

struct Ctx {
    done: AtomicUsize,
    total: usize,
    remote: Remote,
}

fn run(producers: usize, per: usize) -> f64 {
    let mut io = Io::new(Options::default()).unwrap();
    let remote = io.remote();
    let total = producers * per;
    // Leaked so the closures capture one `&'static` pointer, like the C++ twin.
    let ctx: &'static Ctx = Box::leak(Box::new(Ctx {
        done: AtomicUsize::new(0),
        total,
        remote: remote.clone(),
    }));
    let start = Instant::now();
    let threads: Vec<_> = (0..producers)
        .map(|_| {
            let remote = remote.clone();
            thread::spawn(move || {
                for _ in 0..per {
                    while remote
                        .post(move |_| {
                            if ctx.done.fetch_add(1, Ordering::Relaxed) + 1 == ctx.total {
                                ctx.remote.stop();
                            }
                        })
                        .is_err()
                    {
                        thread::yield_now();
                    }
                }
            })
        })
        .collect();
    io.run();
    let ns = start.elapsed().as_nanos() as f64;
    for t in threads {
        t.join().unwrap();
    }
    ns / total as f64
}

fn main() {
    const TOTAL: usize = 400_000;
    println!("post 1 producer: {} ns/op", run(1, TOTAL));
    println!("post 4 producers: {} ns/op", run(4, TOTAL / 4));
}
