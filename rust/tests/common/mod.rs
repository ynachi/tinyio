#![forbid(unsafe_code)]
#![allow(dead_code)]
//! Helpers shared by the integration tests.

use std::path::{Path, PathBuf};
use std::sync::Once;
use std::time::Duration;

/// A hang in a loop test would otherwise stall `cargo test` forever: abort the
/// whole process, loudly, if a test binary runs longer than `secs`.
pub fn watchdog(secs: u64) {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_secs(secs));
            eprintln!("watchdog: test binary still running after {secs}s, aborting (hang?)");
            std::process::abort();
        });
    });
}

/// A scratch directory removed on drop.
pub struct TmpDir(PathBuf);

impl TmpDir {
    pub fn new(tag: &str) -> TmpDir {
        let dir = std::env::temp_dir().join(format!("tinyio-rs-{}-{tag}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        TmpDir(dir)
    }

    pub fn path(&self) -> &Path {
        &self.0
    }

    pub fn join(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}

impl Drop for TmpDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
