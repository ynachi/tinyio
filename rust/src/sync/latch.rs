use std::cell::Cell;
use std::io;
use std::rc::Rc;

use super::waiters::WaitList;
use super::{Attach, bind, check_same};
use crate::check::check;
use crate::cx::Cx;
use crate::detail::fiber::WaitKind;
use crate::sched::Sched;

/// A one-shot countdown: `wait` returns once `count_down` has been called
/// `count` times.
pub struct Latch {
    sched: Rc<Sched>,
    count: Cell<usize>,
    waiters: WaitList,
}

impl std::fmt::Debug for Latch {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Latch")
            .field("count", &self.count.get())
            .finish_non_exhaustive()
    }
}

impl Latch {
    #[inline]
    pub fn new(at: &impl Attach, count: usize) -> Latch {
        Latch {
            sched: bind(at),
            count: Cell::new(count),
            waiters: WaitList::new(WaitKind::Latch),
        }
    }

    /// Aborts if the count is already zero (counting below zero is a bug).
    #[inline]
    pub fn count_down(&self) {
        let n = self.count.get();
        check!(n > 0, "Latch::count_down called more times than its count");
        self.count.set(n - 1);
        if n == 1 {
            self.waiters.wake_all(&self.sched);
        }
    }

    /// Wait for the count to reach zero; returns at once if it already has.
    /// `ECANCELED` if the runtime is stopping and the count never got there.
    #[inline]
    pub fn wait(&self, cx: &Cx) -> io::Result<()> {
        check_same(&self.sched, cx, "Latch");
        if self.count.get() > 0 {
            self.waiters.wait(&self.sched)?;
        }
        Ok(())
    }
}
