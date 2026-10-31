#![forbid(unsafe_code)]
#![allow(dead_code)]
//! Helpers the examples share: `--name=value` flags and a thread-per-core
//! worker pool. `Io` is `!Send`, so each worker builds its own inside its thread.

use std::str::FromStr;
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use tinyio::{Io, Options, Remote};

/// `--name=value` from the command line, or `default`.
pub fn flag<T: FromStr>(name: &str, default: T) -> T {
    let prefix = format!("--{name}=");
    std::env::args()
        .find_map(|a| a.strip_prefix(&prefix).and_then(|v| v.parse().ok()))
        .unwrap_or(default)
}

/// The arguments that are not `--flags`.
pub fn positional() -> Vec<String> {
    std::env::args()
        .skip(1)
        .filter(|a| !a.starts_with("--"))
        .collect()
}

/// Run `workers` threads, each with its own `Io` set up by `setup(&io, index)`,
/// for `seconds` (forever when 0, until the process is killed), then stop them
/// all through their `Remote`s and join.
pub fn run_workers<F>(workers: usize, seconds: u64, setup: F)
where
    F: Fn(&Io, usize) + Send + Sync + 'static,
{
    let setup = std::sync::Arc::new(setup);
    // Each worker reports once: its `Remote`, or `None` if it could not start.
    let (tx, rx) = mpsc::channel::<Option<Remote>>();
    let handles: Vec<_> = (0..workers)
        .map(|i| {
            let (setup, tx) = (setup.clone(), tx.clone());
            thread::spawn(move || {
                let mut io = match Io::new(Options::default()) {
                    Ok(io) => io,
                    Err(e) => {
                        eprintln!("worker {i}: {e}");
                        let _ = tx.send(None);
                        return;
                    }
                };
                setup(&io, i);
                let _ = tx.send(Some(io.remote()));
                io.run(); // returns after Remote::stop, once everything is cancelled and drained
            })
        })
        .collect();
    let remotes: Vec<Remote> = (0..workers)
        .filter_map(|_| rx.recv().ok().flatten())
        .collect();
    if seconds == 0 {
        loop {
            thread::park();
        }
    }
    thread::sleep(Duration::from_secs(seconds));
    for r in &remotes {
        r.stop();
    }
    for h in handles {
        let _ = h.join();
    }
}
