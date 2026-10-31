//! tinyio-rs: stackful fibers over io_uring, one ring per thread.
//!
//! The public API is safe (Rule 0): no sequence of calls to anything exported
//! here can cause undefined behavior. `unsafe` lives only in the modules the
//! plan's unsafe budget lists; see `docs/architecture-plan.md`.
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

mod check;
mod cx;
mod detail;
mod fs;
mod io;
mod options;
mod ring;
mod sched;
#[cfg(test)]
mod testutil;

pub use crate::cx::{Cx, is_cancelled};
pub use crate::detail::fiber::FiberId;
pub use crate::fs::OpenOptions;
pub use crate::io::Io;
pub use crate::options::Options;
