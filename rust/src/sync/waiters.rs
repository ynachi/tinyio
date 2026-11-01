use std::cell::Cell;
use std::io;
use std::ptr::NonNull;

use crate::check::check;
use crate::detail::fiber::WaitKind;
use crate::sched::{FiberQueue, Sched};

const UNREGISTERED: usize = usize::MAX;

/// FIFO of fibers parked on one primitive. Links through `FiberHeader::next`,
/// which is free while a fiber is `Waiting`, so parking allocates nothing.
///
/// A list with waiters is registered with its scheduler (`WaitRegistry`), so
/// shutdown can wake them with `ECANCELED` (invariant 10). `Scope` lists are
/// not: their waiters wait for fibers that finish by themselves.
pub(crate) struct WaitList {
    queue: FiberQueue,
    len: Cell<usize>,
    kind: WaitKind,
    /// Position in the scheduler's `WaitRegistry`, or `UNREGISTERED`.
    reg_index: Cell<usize>,
}

impl WaitList {
    pub(crate) const fn new(kind: WaitKind) -> WaitList {
        WaitList {
            queue: FiberQueue::new(),
            len: Cell::new(0),
            kind,
            reg_index: Cell::new(UNREGISTERED),
        }
    }

    /// Park the running fiber at the back until a `wake_*` pops it. `ECANCELED`
    /// if the runtime is stopping (without waiting) or shuts down while this
    /// fiber waits (it is then no longer in the list).
    pub(crate) fn wait(&self, sched: &Sched) -> io::Result<()> {
        if sched.stopping() {
            return Err(io::Error::from_raw_os_error(libc::ECANCELED));
        }
        self.park(sched);
        let me = sched.current();
        // SAFETY: the running fiber's header is live.
        if unsafe { me.as_ref() }.wait_cancelled.replace(false) {
            return Err(io::Error::from_raw_os_error(libc::ECANCELED));
        }
        Ok(())
    }

    /// Park the running fiber at the back until a `wake_*` pops it. For waits
    /// that shutdown must not cut short (`scope`'s join).
    pub(crate) fn park(&self, sched: &Sched) {
        let me = sched.current();
        // SAFETY: the running fiber is live and in no queue (invariant 5).
        unsafe { self.queue.push(me) };
        let len = self.len.get() + 1;
        self.len.set(len);
        if len == 1 && self.kind != WaitKind::Scope {
            sched.sync_waits().add(self);
        }
        sched.park(self.kind);
    }

    /// Wake the oldest waiter. `false` if there is none. The empty check is
    /// inline so the uncontended path costs no call.
    #[inline]
    pub(crate) fn wake_one(&self, sched: &Sched) -> bool {
        self.len.get() != 0 && self.wake_front(sched)
    }

    #[inline(never)]
    fn wake_front(&self, sched: &Sched) -> bool {
        let Some(fiber) = self.queue.pop() else {
            return false;
        };
        let len = self.len.get() - 1;
        self.len.set(len);
        if len == 0 && self.reg_index.get() != UNREGISTERED {
            sched.sync_waits().remove(self);
        }
        sched.unpark(fiber, self.kind);
        true
    }

    #[inline]
    pub(crate) fn wake_all(&self, sched: &Sched) {
        while self.wake_one(sched) {}
    }

    /// Shutdown: wake every waiter, each told it was cancelled.
    fn cancel_waiters(&self, sched: &Sched) {
        while let Some(fiber) = self.queue.pop() {
            self.len.set(self.len.get() - 1);
            // SAFETY: a queued fiber is a live header of this scheduler.
            unsafe { fiber.as_ref() }.wait_cancelled.set(true);
            sched.unpark(fiber, self.kind);
        }
        sched.sync_waits().remove(self);
    }
}

/// The lists that have waiters right now, so shutdown can find them.
pub(crate) struct WaitRegistry {
    lists: Cell<Vec<NonNull<WaitList>>>,
}

impl WaitRegistry {
    pub(crate) const fn new() -> WaitRegistry {
        WaitRegistry {
            lists: Cell::new(Vec::new()),
        }
    }

    fn add(&self, list: &WaitList) {
        let mut lists = self.lists.take();
        list.reg_index.set(lists.len());
        lists.push(NonNull::from(list));
        self.lists.set(lists);
    }

    fn remove(&self, list: &WaitList) {
        let i = list.reg_index.replace(UNREGISTERED);
        if i == UNREGISTERED {
            return;
        }
        let mut lists = self.lists.take();
        lists.swap_remove(i);
        if let Some(moved) = lists.get(i) {
            // SAFETY: registered lists have waiters, hence a live primitive.
            unsafe { moved.as_ref() }.reg_index.set(i);
        }
        self.lists.set(lists);
    }

    /// Shutdown step: wake every sync waiter with `ECANCELED`. After this
    /// `WaitList::wait` refuses (the runtime is stopping), so none comes back.
    pub(crate) fn cancel_all(&self, sched: &Sched) {
        loop {
            let lists = self.lists.take();
            let last = lists.last().copied();
            self.lists.set(lists);
            let Some(list) = last else { break };
            // SAFETY: a registered list has waiters, and a waiter borrows its
            // primitive, so the list is alive; `cancel_waiters` deregisters it.
            unsafe { list.as_ref() }.cancel_waiters(sched);
        }
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

    // Prediction: shutdown's `cancel_all` wakes the waiters of every registered
    // list, oldest first, each marked cancelled; the registry and the lists end
    // up empty and unregistered, including after a swap-remove moved an entry.
    #[test]
    fn cancel_all_wakes_every_registered_list() {
        let sched = Sched::new(Options::default().validate().unwrap());
        let (l1, l2, l3) = (
            WaitList::new(WaitKind::Mutex),
            WaitList::new(WaitKind::Mutex),
            WaitList::new(WaitKind::Mutex),
        );
        let headers = [waiting_header(), waiting_header(), waiting_header()];
        for (list, h) in [&l1, &l2, &l3].into_iter().zip(headers) {
            // SAFETY: live headers in no queue.
            unsafe { list.queue.push(h) };
            list.len.set(1);
            sched.sync_waits().add(list);
        }
        // The first list drains by an ordinary wake: swap-remove moves l3 to slot 0.
        assert!(l1.wake_one(&sched));
        assert_eq!(l1.reg_index.get(), UNREGISTERED);
        assert_eq!(l3.reg_index.get(), 0);
        let _ = sched.pop_ready_for_test(); // l1's waiter, woken normally

        sched.sync_waits().cancel_all(&sched);
        for list in [&l2, &l3] {
            assert_eq!(list.len.get(), 0);
            assert_eq!(list.reg_index.get(), UNREGISTERED);
        }
        let woken = [sched.pop_ready_for_test(), sched.pop_ready_for_test()];
        assert_eq!(woken.iter().flatten().count(), 2);
        for h in woken.into_iter().flatten() {
            // SAFETY: live test headers.
            assert!(unsafe { h.as_ref() }.wait_cancelled.get());
        }
        assert!(sched.pop_ready_for_test().is_none());
        for h in headers {
            // SAFETY: allocated by `waiting_header`, unqueued by now.
            drop(unsafe { Box::from_raw(h.as_ptr()) });
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
