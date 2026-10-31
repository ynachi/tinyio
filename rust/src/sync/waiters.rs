use std::cell::Cell;

use crate::check::check;
use crate::detail::fiber::WaitKind;
use crate::sched::{FiberQueue, Sched};

/// FIFO of fibers parked on one primitive. Links through `FiberHeader::next`,
/// which is free while a fiber is `Waiting`, so parking allocates nothing.
pub(super) struct WaitList {
    queue: FiberQueue,
    len: Cell<usize>,
    kind: WaitKind,
}

impl WaitList {
    pub(super) const fn new(kind: WaitKind) -> WaitList {
        WaitList {
            queue: FiberQueue::new(),
            len: Cell::new(0),
            kind,
        }
    }

    /// Park the running fiber at the back until a `wake_*` pops it.
    pub(super) fn park(&self, sched: &Sched) {
        let me = sched.current();
        // SAFETY: the running fiber is live and in no queue (invariant 5).
        unsafe { self.queue.push(me) };
        self.len.set(self.len.get() + 1);
        sched.park(self.kind);
    }

    /// Wake the oldest waiter. `false` if there is none. The empty check is
    /// inline so the uncontended path costs no call.
    #[inline]
    pub(super) fn wake_one(&self, sched: &Sched) -> bool {
        self.len.get() != 0 && self.wake_front(sched)
    }

    #[inline(never)]
    fn wake_front(&self, sched: &Sched) -> bool {
        let Some(fiber) = self.queue.pop() else {
            return false;
        };
        self.len.set(self.len.get() - 1);
        sched.unpark(fiber, self.kind);
        true
    }

    #[inline]
    pub(super) fn wake_all(&self, sched: &Sched) {
        while self.wake_one(sched) {}
    }
}

impl Drop for WaitList {
    // Second line of defence: a parked fiber borrows its primitive, so safe
    // code cannot get here.
    #[inline]
    fn drop(&mut self) {
        check!(
            self.len.get() == 0,
            "{} dropped with {} fiber(s) still waiting on it",
            self.kind.name(),
            self.len.get()
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::detail::fiber::{FiberHeader, FiberState};
    use crate::options::Options;
    use crate::testutil::death_test;
    use std::ptr::NonNull;

    fn waiting_header() -> NonNull<FiberHeader> {
        let h = NonNull::from(Box::leak(FiberHeader::detached()));
        // SAFETY: just allocated, live.
        let hr = unsafe { h.as_ref() };
        hr.state.set(FiberState::Waiting);
        hr.wait.set(WaitKind::Mutex);
        h
    }

    // Prediction: waiters wake oldest first, and each wake moves the fiber to the ready queue.
    #[test]
    fn wakes_in_fifo_order() {
        let sched = Sched::new(Options::default().validate().unwrap());
        let list = WaitList::new(WaitKind::Mutex);
        let (a, b) = (waiting_header(), waiting_header());
        // SAFETY: live headers in no queue.
        unsafe {
            list.queue.push(a);
            list.queue.push(b);
        }
        list.len.set(2);
        assert!(list.wake_one(&sched));
        assert!(list.wake_one(&sched));
        assert!(!list.wake_one(&sched));
        assert_eq!(sched.pop_ready_for_test(), Some(a));
        assert_eq!(sched.pop_ready_for_test(), Some(b));
        // SAFETY: both were allocated by `waiting_header` and are unqueued now.
        unsafe {
            drop(Box::from_raw(a.as_ptr()));
            drop(Box::from_raw(b.as_ptr()));
        }
    }

    // Prediction: dropping a list that still has a waiter aborts and names the primitive.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_drop_with_waiters() {
        death_test(
            "sync::waiters::tests::death_drop_with_waiters",
            || {
                let list = WaitList::new(WaitKind::Semaphore);
                // SAFETY: a live header in no queue; the process aborts below.
                unsafe { list.queue.push(waiting_header()) };
                list.len.set(1);
                drop(list);
            },
            &["Semaphore dropped with 1 fiber(s) still waiting"],
        );
    }
}
