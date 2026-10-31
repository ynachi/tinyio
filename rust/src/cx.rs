#![forbid(unsafe_code)]
//! `Cx`: what a fiber holds. It carries the scheduler handle and nothing else
//! (Rule 13: a fiber's identity is `sched.current`, never the `Cx` it was
//! given, so a scoped child may legally hold its parent's `&Cx`).
//!
//! The unsafe budget lists `cx.rs` for stage 2's `syscall`; stage 1 has none.

use std::io;
use std::marker::PhantomData;
use std::rc::Rc;

use crate::detail::fiber::FiberId;
use crate::sched::Sched;

/// Marker that makes a type `!Send + !Sync` (Rule 1).
pub(crate) type NotSend = PhantomData<*const ()>;

/// The handle a fiber uses to reach its runtime: spawn, yield, and (from
/// stage 2) perform I/O. `!Send`: it never leaves the thread of its `Io`.
pub struct Cx {
    sched: Rc<Sched>,
    _p: NotSend,
}

impl Cx {
    pub(crate) fn new(sched: Rc<Sched>) -> Cx {
        Cx {
            sched,
            _p: PhantomData,
        }
    }

    #[cfg(test)]
    pub(crate) fn sched(&self) -> &Rc<Sched> {
        &self.sched
    }

    /// Start another fiber on this thread. It runs after the current one
    /// next suspends. `f` must own everything it uses (`'static`); it cannot
    /// capture this `Cx`, and receives its own.
    pub fn spawn<F>(&self, f: F) -> io::Result<FiberId>
    where
        F: FnOnce(&Cx) + 'static,
    {
        self.sched.spawn(f)
    }

    /// Give the thread to other ready fibers; resume after they have had a turn.
    pub fn yield_now(&self) -> io::Result<()> {
        self.sched.yield_current();
        Ok(())
    }
}

/// `true` if `e` is the error a call returns when the runtime is shutting
/// down (`ECANCELED`). It has no `io::ErrorKind`, so match the raw code.
pub fn is_cancelled(e: &io::Error) -> bool {
    e.raw_os_error() == Some(libc::ECANCELED)
}
