#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Layer 4 (public-API side): the test re-executes its own binary with
//! `TINYIO_CASE=<name>` and asserts an abort and the expected stderr text.

use std::os::unix::process::ExitStatusExt;
use std::process::Command;

use tinyio::{Io, Options};

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
