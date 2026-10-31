use std::cell::Cell;
use std::rc::Rc;

use super::waiters::WaitList;
use super::{Attach, bind, check_same};
use crate::check::check;
use crate::cx::Cx;
use crate::detail::fiber::WaitKind;
use crate::sched::Sched;

/// A counting gate: at most `permits` fibers hold a permit at once.
pub struct Semaphore {
    sched: Rc<Sched>,
    permits: Cell<usize>,
    waiters: WaitList,
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

    /// Take a permit, waiting in FIFO order if none is free.
    #[inline]
    pub fn acquire(&self, cx: &Cx) {
        check_same(&self.sched, cx, "Semaphore");
        match self.permits.get() {
            // `release` hands its permit straight to a waiter, so a woken fiber has one.
            0 => self.waiters.park(&self.sched),
            n => self.permits.set(n - 1),
        }
    }

    #[inline]
    pub fn try_acquire(&self) -> bool {
        match self.permits.get() {
            0 => false,
            n => {
                self.permits.set(n - 1);
                true
            }
        }
    }

    #[inline]
    pub fn release(&self) {
        if !self.waiters.wake_one(&self.sched) {
            let n = self.permits.get();
            check!(n < usize::MAX, "Semaphore permit count overflow");
            self.permits.set(n + 1);
        }
    }
}
