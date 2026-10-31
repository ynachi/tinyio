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
