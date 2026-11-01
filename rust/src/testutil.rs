#![forbid(unsafe_code)]
//! Test helpers. `death_test` re-executes the test binary to observe an abort
//! (the plan's layer 4: "TINYIO_CASE=<name>, assert an abort signal and the
//! expected stderr text").

use std::os::unix::process::ExitStatusExt;
use std::process::Command;

/// Run `body` in a child process and assert it dies by SIGABRT with
/// `expect` somewhere in stderr. In the child (`TINYIO_CASE == name`) it runs
/// `body` directly; surviving it is a failure the parent reports.
pub(crate) fn death_test(name: &str, body: fn(), expect: &[&str]) {
    if std::env::var("TINYIO_CASE").as_deref() == Ok(name) {
        body();
        std::process::exit(0);
    }
    let exe = std::env::current_exe().unwrap();
    let out = Command::new(exe)
        .args([name, "--exact", "--nocapture", "--test-threads=1"])
        .env("TINYIO_CASE", name)
        .output()
        .unwrap();
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert_eq!(
        out.status.signal(),
        Some(libc::SIGABRT),
        "expected SIGABRT, got {:?}; stderr:\n{stderr}",
        out.status
    );
    for needle in expect {
        assert!(
            stderr.contains(needle),
            "stderr lacks {needle:?}:\n{stderr}"
        );
    }
}

/// Run `body` in a child process and assert it exits with status 0. For
/// bodies that change process-wide state (signal dispositions) or that would
/// kill the test binary if they went wrong.
pub(crate) fn child_test(name: &str, body: fn()) {
    if std::env::var("TINYIO_CASE").as_deref() == Ok(name) {
        body();
        std::process::exit(0);
    }
    let exe = std::env::current_exe().unwrap();
    let out = Command::new(exe)
        .args([name, "--exact", "--nocapture", "--test-threads=1"])
        .env("TINYIO_CASE", name)
        .output()
        .unwrap();
    assert!(
        out.status.success(),
        "child {name} failed: {:?}\n{}",
        out.status,
        String::from_utf8_lossy(&out.stderr)
    );
}

/// Abort the process if a test binary runs for more than 60 s: a hung drain
/// would otherwise stall `cargo test` forever.
pub(crate) fn watchdog() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        std::thread::spawn(|| {
            std::thread::sleep(std::time::Duration::from_secs(60));
            eprintln!("watchdog: unit tests still running after 60s, aborting (hang?)");
            std::process::abort();
        });
    });
}
