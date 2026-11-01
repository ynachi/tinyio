#![forbid(unsafe_code)]
use std::cell::Cell;
use std::io;
use std::rc::Rc;

use super::waiters::WaitList;
use super::{Attach, bind, check_same};
use crate::check::check;
use crate::cx::Cx;
use crate::detail::fiber::WaitKind;
use crate::sched::Sched;

/// A counting gate: at most as many fibers hold a [`Permit`] as it has permits.
pub struct Semaphore {
    sched: Rc<Sched>,
    permits: Cell<usize>,
    waiters: WaitList,
}

/// A permit taken from a [`Semaphore`]; dropping it gives the permit back.
/// Because the return is tied to the value, an early `return` or `?` cannot
/// leak it. [`forget`](Permit::forget) keeps the permit taken, for a hand-off
/// that a different fiber completes with [`Semaphore::release`].
#[must_use = "dropping the permit releases it at once"]
pub struct Permit<'a> {
    sem: &'a Semaphore,
}

impl std::fmt::Debug for Semaphore {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Semaphore")
            .field("available", &self.permits.get())
            .finish_non_exhaustive()
    }
}

impl std::fmt::Debug for Permit<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Permit").finish_non_exhaustive()
    }
}

impl Permit<'_> {
    /// Keep the permit taken: it is no longer given back on drop.
    pub fn forget(self) {
        std::mem::forget(self);
    }
}

impl Drop for Permit<'_> {
    #[inline]
    fn drop(&mut self) {
        self.sem.release();
    }
}

impl Semaphore {
    #[inline]
    pub fn new(at: &impl Attach, permits: usize) -> Semaphore {
        Semaphore {
            sched: bind(at),
            permits: Cell::new(permits),
            waiters: WaitList::new(WaitKind::Semaphore),
        }
    }

    /// Take a permit, waiting in FIFO order if none is free. `ECANCELED` if the
    /// runtime is stopping and no permit was free.
    #[inline]
    pub fn acquire(&self, cx: &Cx) -> io::Result<Permit<'_>> {
        check_same(&self.sched, cx, "Semaphore");
        match self.permits.get() {
            // `release` hands its permit straight to a waiter, so a woken fiber has one.
            0 => self.waiters.wait(&self.sched)?,
            n => self.permits.set(n - 1),
        }
        Ok(Permit { sem: self })
    }

    /// Take a permit if one is free.
    #[must_use = "dropping the permit releases it at once"]
    #[inline]
    pub fn try_acquire(&self) -> Option<Permit<'_>> {
        match self.permits.get() {
            0 => None,
            n => {
                self.permits.set(n - 1);
                Some(Permit { sem: self })
            }
        }
    }

    /// Add a permit, or hand it to the oldest waiter. This is how a
    /// [`forgotten`](Permit::forget) permit comes back, and also lets a
    /// semaphore start at zero and be used as a counting signal.
    #[inline]
    pub fn release(&self) {
        if !self.waiters.wake_one(&self.sched) {
            let n = self.permits.get();
            check!(n < usize::MAX, "Semaphore permit count overflow");
            self.permits.set(n + 1);
        }
    }
}
