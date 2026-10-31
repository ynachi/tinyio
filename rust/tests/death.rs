#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Layer 4 (public-API side): the test re-executes its own binary with
//! `TINYIO_CASE=<name>` and asserts an abort and the expected stderr text.

use std::os::unix::process::ExitStatusExt;
use std::process::Command;

use tinyio::{Baton, Io, Latch, Mutex, Options};

const SIGABRT: i32 = 6;
const SIGSEGV: i32 = 11;

fn death_test(name: &str, body: fn(), expect: &[&str]) {
    death_test_signal(name, body, SIGABRT, expect);
}

fn death_test_signal(name: &str, body: fn(), signal: i32, expect: &[&str]) {
    if std::env::var("TINYIO_CASE").as_deref() == Ok(name) {
        body();
        std::process::exit(0);
    }
    let out = Command::new(std::env::current_exe().unwrap())
        .args([name, "--exact", "--nocapture", "--test-threads=1"])
        .env("TINYIO_CASE", name)
        .output()
        .unwrap();
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert_eq!(
        out.status.signal(),
        Some(signal),
        "{:?}\n{stderr}",
        out.status
    );
    for needle in expect {
        assert!(
            stderr.contains(needle),
            "stderr lacks {needle:?}:\n{stderr}"
        );
    }
}

// Prediction (Rule 8): a panic escaping a fiber aborts the process (SIGABRT)
// and the panic message reaches stderr first, so it is readable.
#[test]
fn panic_in_fiber_aborts_with_a_readable_message() {
    death_test(
        "panic_in_fiber_aborts_with_a_readable_message",
        || {
            let mut io = Io::new(Options::default()).unwrap();
            io.spawn(|_| panic!("boom from fiber")).unwrap();
            io.run_until_done();
        },
        &["boom from fiber"],
    );
}

// Prediction: dropping an Io after run_until_done is clean (exit 0, no abort).
#[test]
fn drop_after_run_until_done_is_clean() {
    let mut io = Io::new(Options::default()).unwrap();
    io.spawn(|cx| {
        cx.yield_now().unwrap();
    })
    .unwrap();
    io.run_until_done();
    drop(io);
}

fn recurse(n: u64) -> u64 {
    let pad = [n as u8; 1024];
    std::hint::black_box(&pad);
    if n == 0 {
        0
    } else {
        recurse(n - 1) + u64::from(pad[3])
    }
}

// Prediction: runaway recursion on a fiber runs into the PROT_NONE guard
// region and dies with SIGSEGV (frames are 1 KiB, far below the 64 KiB guard,
// so the guard cannot be skipped). It must not corrupt a neighbouring stack.
#[test]
fn fiber_stack_overflow_is_sigsegv() {
    death_test_signal(
        "fiber_stack_overflow_is_sigsegv",
        || {
            let mut io = Io::new(Options::default()).unwrap();
            io.spawn(|_| {
                std::hint::black_box(recurse(1_000_000));
            })
            .unwrap();
            io.run_until_done();
        },
        SIGSEGV,
        &[],
    );
}

fn run_fibers(setup: impl FnOnce(&Io)) {
    let mut io = Io::new(Options::default()).unwrap();
    setup(&io);
    io.run_until_done();
}

// Prediction (invariant 10): a fiber parked on a Mutex nobody will release
// aborts the quiesce, naming Mutex and the fiber (fiber 1 leaks the guard).
#[test]
fn quiesce_with_a_fiber_parked_on_a_mutex() {
    death_test(
        "quiesce_with_a_fiber_parked_on_a_mutex",
        || {
            run_fibers(|io| {
                let m = std::rc::Rc::new(Mutex::new(io, ()));
                let m1 = m.clone();
                io.spawn(move |cx| std::mem::forget(m1.lock(cx))).unwrap();
                io.spawn(move |cx| drop(m.lock(cx))).unwrap();
            });
        },
        &["1 on Mutex (fiber 2)"],
    );
}

// Prediction: two stuck primitives are both named.
#[test]
fn quiesce_names_every_stuck_primitive() {
    death_test(
        "quiesce_names_every_stuck_primitive",
        || {
            run_fibers(|io| {
                let m = std::rc::Rc::new(Mutex::new(io, ()));
                let latch = std::rc::Rc::new(Latch::new(io, 1));
                let m1 = m.clone();
                io.spawn(move |cx| std::mem::forget(m1.lock(cx))).unwrap();
                io.spawn(move |cx| drop(m.lock(cx))).unwrap();
                io.spawn(move |cx| latch.wait(cx)).unwrap();
            });
        },
        &["1 on Mutex (fiber 2)", "1 on Latch (fiber 3)"],
    );
}

// Prediction (Rule 13): a primitive made on one Io, used from a fiber of another, aborts.
#[test]
fn primitive_used_from_another_io() {
    death_test(
        "primitive_used_from_another_io",
        || {
            let io1 = Io::new(Options::default()).unwrap();
            let m = std::rc::Rc::new(Mutex::new(&io1, ()));
            let mut io2 = Io::new(Options::default()).unwrap();
            io2.spawn(move |cx| drop(m.lock(cx))).unwrap();
            io2.run_until_done();
        },
        &["Mutex used from a different Io"],
    );
}

// Prediction: a second concurrent waiter on a one-shot Baton aborts.
#[test]
fn baton_second_waiter() {
    death_test(
        "baton_second_waiter",
        || {
            run_fibers(|io| {
                let b = std::rc::Rc::new(Baton::new(io));
                let b2 = b.clone();
                io.spawn(move |cx| b.wait(cx)).unwrap();
                io.spawn(move |cx| b2.wait(cx)).unwrap();
            });
        },
        &["Baton: second concurrent wait"],
    );
}

// Prediction: counting a latch below zero aborts.
#[test]
fn latch_counted_below_zero() {
    death_test(
        "latch_counted_below_zero",
        || {
            run_fibers(|io| {
                let l = Latch::new(io, 1);
                io.spawn(move |_| {
                    l.count_down();
                    l.count_down();
                })
                .unwrap();
            });
        },
        &["more times than its count"],
    );
}

// Prediction: a panic caught outside `scope` while a child still runs would
// unwind the frames the child borrows, so it aborts instead.
#[test]
fn panic_through_scope_with_a_live_child() {
    death_test(
        "panic_through_scope_with_a_live_child",
        || {
            run_fibers(|io| {
                io.spawn(|cx| {
                    let caught = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        tinyio::scope(cx, |s| {
                            s.spawn(|_| {}).unwrap();
                            std::panic::resume_unwind(Box::new("boom"));
                        });
                    }));
                    drop(caught);
                })
                .unwrap();
            });
        },
        &["tinyio::scope left with 1 child fiber(s) still running"],
    );
}

// Prediction (invariant 10): `stop` with a fiber parked on a Mutex nobody will
// release finishes the drain, then aborts naming the Mutex. The fiber is never
// woken or unwound.
#[test]
fn stop_with_a_sync_parked_fiber() {
    death_test(
        "stop_with_a_sync_parked_fiber",
        || {
            let mut io = Io::new(Options::default()).unwrap();
            let m = std::rc::Rc::new(Mutex::new(&io, ()));
            let m1 = m.clone();
            io.spawn(move |cx| std::mem::forget(m1.lock(cx))).unwrap();
            io.spawn(move |cx| drop(m.lock(cx))).unwrap();
            io.remote().stop();
            io.run();
        },
        &["cancel_and_drain: quiesced", "1 on Mutex (fiber 2)"],
    );
}
