//! tinyio-rs: stackful fibers over io_uring, one ring per thread.
//!
//! Fibers are ordinary closures with their own stacks, so I/O reads like
//! blocking code (`let n = cx.read(&fd, &mut buf, None)?`) while the thread runs
//! other fibers. An [`Io`] owns the ring and runs the loop; a [`Cx`] is what a
//! fiber holds to do I/O; a [`Remote`] is the one handle other threads may use.
//!
//! The public API is safe: no sequence of calls to anything exported here can
//! cause undefined behavior. `unsafe` is confined to the modules that switch
//! stacks, talk to the ring, and wrap a few libc calls.
//!
//! # Requirements
//!
//! Linux 6.9 or later (x86_64 or aarch64). 6.9 added `IORING_OP_FTRUNCATE`, the
//! newest opcode the crate submits, and there is no fallback for an older
//! kernel. [`Io::new`] asks the kernel which opcodes it supports (a probe, not a
//! version string: distributions backport) and returns an error of kind
//! [`Unsupported`](std::io::ErrorKind::Unsupported) that names the missing
//! opcode, so a caller can report it.
//!
//! # Shutdown
//!
//! [`Remote::stop`] makes [`Io::run`] cancel everything and return. I/O in
//! flight completes with `ECANCELED` ([`is_cancelled`]); a fiber waiting on a
//! [`Mutex`], [`Semaphore`], [`Latch`] or [`Baton`] is woken with `ECANCELED`
//! too, and every later blocking call fails the same way, so a fiber that checks
//! its results runs to its end. A lock that is free is still taken while
//! stopping, so cleanup code can use it.
//!
//! # What aborts the process
//!
//! A panic in a fiber does not abort: the runtime stops and drains, then
//! [`Io::run`] and [`Io::run_until_done`] resume the panic on the caller's
//! thread. The process aborts, with a message that names the cause, for a
//! broken invariant the types cannot rule out:
//!
//! - a fiber left waiting for something that can no longer happen (a re-entrant
//!   `Mutex::lock`, a `Latch` whose count is never reached) once the loop has
//!   nothing else to run,
//! - a second concurrent waiter on a [`Baton`], or counting a [`Latch`] below zero,
//! - using a primitive from an [`Io`] other than the one it was made on,
//! - a [`scope`] unwound by a panic while a child fiber still borrows it,
//! - dropping an [`Io`] with operations in flight.
//!
//! A fiber that overflows its stack faults on the guard page (SIGSEGV).
//!
//! # Compile-time guarantees
//!
//! `Io` and `Cx` never leave their thread:
//!
//! ```compile_fail
//! fn needs_send<T: Send>() {}
//! needs_send::<tinyio::Io>();
//! ```
//! ```
//! fn needs_send<T: Send>() {}
//! needs_send::<Vec<u8>>(); // the twin: the helper itself is fine
//! ```
//!
//! ```compile_fail
//! fn needs_send<T: Send>() {}
//! needs_send::<tinyio::Cx>();
//! ```
//!
//! A spawned fiber cannot capture the `Cx` it was spawned from; it receives
//! its own:
//!
//! ```compile_fail,E0521
//! let mut io = tinyio::Io::new(Default::default()).unwrap();
//! io.spawn(|cx| {
//!     cx.spawn(|_| { let _outer = &cx; }).ok(); // captures the outer cx
//! }).unwrap();
//! io.run_until_done();
//! ```
//! ```
//! let mut io = tinyio::Io::new(Default::default()).unwrap();
//! io.spawn(|cx| {
//!     cx.spawn(|_cx| {}).ok(); // the twin: uses the inner one
//! }).unwrap();
//! io.run_until_done();
//! ```
//!
//! A plain `spawn` cannot borrow the parent's locals:
//!
//! ```compile_fail
//! let mut io = tinyio::Io::new(Default::default()).unwrap();
//! let mut n = 0;
//! io.spawn(|_| n += 1).unwrap();
//! io.run_until_done();
//! ```
//! ```
//! let mut io = tinyio::Io::new(Default::default()).unwrap();
//! let n = std::rc::Rc::new(std::cell::Cell::new(0));
//! let m = n.clone();
//! io.spawn(move |_| m.set(m.get() + 1)).unwrap();
//! io.run_until_done();
//! assert_eq!(n.get(), 1);
//! ```

#![deny(unsafe_op_in_unsafe_fn)]
#![cfg_attr(test, allow(clippy::unwrap_used, clippy::expect_used, clippy::panic))]

#[cfg(not(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
)))]
compile_error!(
    "tinyio supports Linux on x86_64 and aarch64 only: it needs io_uring, and the fiber context switch is hand-written assembly for those two CPUs"
);

mod check;
mod cx;
mod detail;
mod fs;
mod io;
mod kernel;
pub mod net;
mod options;
mod remote;
mod ring;
mod sched;
mod sync;
#[cfg(test)]
mod testutil;
mod timer;

pub use crate::cx::{Cx, Timed, is_cancelled};
pub use crate::detail::fiber::FiberId;
pub use crate::fs::{Metadata, OpenOptions};
pub use crate::io::Io;
pub use crate::options::Options;
pub use crate::remote::{PostError, Remote};
pub use crate::sync::{Attach, Baton, Latch, Mutex, MutexGuard, Permit, Scope, Semaphore, scope};
