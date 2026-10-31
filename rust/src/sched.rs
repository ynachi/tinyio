//! The scheduler: ready queue, fiber registry, spawn / resume / suspend /
//! finish. Single thread (Rules 1, 2), no `RefCell` (Rule 5).
//!
//! Every container here is `Cell`-based and intrusive where it can be: the
//! ready queue and the idle list thread through `FiberHeader::next`, so
//! queueing a fiber never allocates. No reference into this state is held
//! across a context switch.
//!
//! # The fiber lifecycle
//!
//! ```text
//!  spawn            resume                 yield / park           finish
//!  ----- Ready ---------> Running -----------------> Ready/Waiting      Running --> Done --> idle list (or unmapped)
//!          ^                                            |
//!          +-------------- push_ready (one way in) <---+
//! ```

use std::cell::Cell;
use std::ffi::c_void;
use std::io;
use std::ptr::NonNull;
use std::rc::Rc;

use crate::check::{check, dcheck, fatal};
use crate::cx::Cx;
use crate::detail::context::{self, Context};
use crate::detail::fiber::{FiberHeader, FiberId, FiberState, TaskOps, WaitKind};
use crate::detail::stack::Stack;
use crate::options::Validated;

/// Closures needing more alignment than the header are refused.
const MAX_TASK_ALIGN: usize = 64;

type Hdr = NonNull<FiberHeader>;

/// An intrusive FIFO of fibers through `FiberHeader::next`.
///
/// Invariant 5 is what makes one link per header enough: a fiber is in the
/// queue at most once, and only `Sched` pushes (through `push_ready`, `spawn`
/// and `yield_current`).
struct ReadyQueue {
    head: Cell<Option<Hdr>>,
    tail: Cell<Option<Hdr>>,
}

impl ReadyQueue {
    const fn new() -> ReadyQueue {
        ReadyQueue {
            head: Cell::new(None),
            tail: Cell::new(None),
        }
    }

    fn is_empty(&self) -> bool {
        self.head.get().is_none()
    }

    /// # Safety
    /// `h` is a live header that is in no queue.
    unsafe fn push(&self, h: Hdr) {
        // SAFETY: fn contract: `h` is live.
        let hr = unsafe { h.as_ref() };
        dcheck!(hr.next.get().is_none(), "fiber pushed while still linked");
        match self.tail.get() {
            // SAFETY: every queued header is live (queued fibers are never
            // freed: only Done or never-queued ones are), so `t` is too.
            Some(t) => unsafe { t.as_ref() }.next.set(Some(h)),
            None => self.head.set(Some(h)),
        }
        self.tail.set(Some(h));
    }

    fn pop(&self) -> Option<Hdr> {
        let h = self.head.get()?;
        // SAFETY: queued headers are live (see `push`).
        let next = unsafe { h.as_ref() }.next.take();
        self.head.set(next);
        if next.is_none() {
            self.tail.set(None);
        }
        Some(h)
    }

    /// Forget every entry without touching the headers (teardown only: the
    /// headers are about to be freed).
    fn forget_all(&self) {
        self.head.set(None);
        self.tail.set(None);
    }
}

pub(crate) struct Sched {
    /// The scheduler's own context: where every fiber switches back to.
    sched_ctx: Context,
    current: Cell<Option<Hdr>>,
    ready: ReadyQueue,
    /// LIFO stack of finished fibers kept for reuse, linked through `next`.
    idle_head: Cell<Option<Hdr>>,
    idle_len: Cell<usize>,
    /// Every fiber, idle ones included (the abort report and teardown need it).
    registry: Cell<Vec<Hdr>>,
    /// Spawned and not yet finished (never-started ones count).
    live: Cell<usize>,
    next_id: Cell<u64>,
    opts: Validated,
}

impl Sched {
    pub(crate) fn new(opts: Validated) -> Rc<Sched> {
        Rc::new(Sched {
            sched_ctx: Context::empty(),
            current: Cell::new(None),
            ready: ReadyQueue::new(),
            idle_head: Cell::new(None),
            idle_len: Cell::new(0),
            registry: Cell::new(Vec::new()),
            live: Cell::new(0),
            next_id: Cell::new(1),
            opts,
        })
    }

    pub(crate) fn opts(&self) -> &Validated {
        &self.opts
    }

    /// Invariant 9: nothing ready (and, from stage 2 on, nothing in flight and
    /// nothing posted). Live fibers do not count.
    pub(crate) fn quiesced(&self) -> bool {
        self.ready.is_empty()
    }

    // ---------------------------------------------------------------- spawn

    pub(crate) fn spawn<F>(&self, f: F) -> io::Result<FiberId>
    where
        F: FnOnce(&Cx) + 'static,
    {
        let size = size_of::<F>();
        let align = align_of::<F>();
        if align > MAX_TASK_ALIGN {
            return Err(io::Error::from_raw_os_error(libc::EINVAL));
        }
        // Plan decision 3: a closure larger than half the stack is ENOMEM.
        if size > self.opts.stack_size / 2 {
            return Err(io::Error::from_raw_os_error(libc::ENOMEM));
        }
        let hdr = self.acquire()?;
        // SAFETY: `acquire` returns a live header owned by this scheduler.
        let h = unsafe { hdr.as_ref() };

        // The closure goes just below the header, aligned for its type.
        let task_ptr = hdr
            .as_ptr()
            .cast::<u8>()
            .map_addr(|a| (a - size) & !(align - 1));
        // SAFETY: `task_ptr..task_ptr+size` lies inside this fiber's usable
        // region (size <= usable/2 and the header is at the very top), is
        // aligned for `F`, and nothing else uses it: the fiber is new or idle.
        unsafe { task_ptr.cast::<F>().write(f) };
        h.task_ptr.set(task_ptr);
        h.task.set(Some(TaskOps::of::<F>()));

        let id = self.next_id.get();
        self.next_id.set(id + 1);
        h.id.set(id);
        h.wait.set(WaitKind::None);

        // SAFETY: the region below `task_ptr` is this fiber's stack; it holds
        // far more than one frame (stack_size >= 4 pages, closure <= half).
        unsafe {
            h.context
                .make(task_ptr, fiber_entry, hdr.as_ptr().cast::<c_void>())
        };

        self.live.set(self.live.get() + 1);
        h.state.set(FiberState::Ready);
        // SAFETY: `hdr` is live and in no queue (new, or taken off the idle list).
        unsafe { self.ready.push(hdr) };
        Ok(FiberId(id))
    }

    /// An idle fiber, or a freshly mapped one.
    fn acquire(&self) -> io::Result<Hdr> {
        if let Some(h) = self.idle_head.get() {
            // SAFETY: idle fibers are live headers.
            let hr = unsafe { h.as_ref() };
            self.idle_head.set(hr.next.take());
            self.idle_len.set(self.idle_len.get() - 1);
            return Ok(h);
        }
        let stack = Stack::map(self.opts.stack_size, self.opts.guard_size)?;
        let hdr_ptr = stack
            .high()
            .wrapping_sub(size_of::<FiberHeader>())
            .cast::<FiberHeader>();
        // SAFETY: the top `size_of::<FiberHeader>()` bytes of the usable
        // region are writable, unused, and 64-aligned (high is page-aligned,
        // the size is a multiple of 64).
        unsafe { hdr_ptr.write(FiberHeader::new(self, stack)) };
        let Some(hdr) = NonNull::new(hdr_ptr) else {
            fatal(format_args!("fiber header address is null"));
        };
        let mut registry = self.registry.take();
        // SAFETY: just written above.
        unsafe { hdr.as_ref() }.registry_index.set(registry.len());
        registry.push(hdr);
        self.registry.set(registry);
        Ok(hdr)
    }

    // ------------------------------------------------------------ the queue

    /// Rule 4: the way a parked fiber becomes ready. Does nothing if it is
    /// already `Ready`; aborts if it is `Running` or `Done`.
    pub(crate) fn push_ready(&self, hdr: Hdr) {
        // SAFETY: callers hold headers of fibers owned by this scheduler.
        let h = unsafe { hdr.as_ref() };
        match h.state.get() {
            FiberState::Ready => {}
            FiberState::Waiting => {
                h.state.set(FiberState::Ready);
                h.wait.set(WaitKind::None);
                // SAFETY: Waiting fibers are in no queue.
                unsafe { self.ready.push(hdr) };
            }
            FiberState::Running => fatal(format_args!(
                "push_ready on running fiber {} (Rule 13)",
                h.id.get()
            )),
            FiberState::Done => fatal(format_args!(
                "push_ready on finished fiber {} (invariant 5)",
                h.id.get()
            )),
        }
    }

    /// The fiber running right now (Rule 13: a fiber's identity is this, never
    /// the `Cx` it holds).
    pub(crate) fn current(&self) -> Hdr {
        match self.current.get() {
            Some(h) => h,
            None => fatal(format_args!("no fiber is running on this scheduler")),
        }
    }

    // --------------------------------------------------- resume and suspend

    /// Resume up to `max` ready fibers; returns how many ran.
    pub(crate) fn resume_ready(&self, max: usize) -> usize {
        let mut n = 0;
        while n < max {
            let Some(hdr) = self.ready.pop() else { break };
            self.resume(hdr);
            n += 1;
        }
        n
    }

    fn resume(&self, hdr: Hdr) {
        // SAFETY: popped from the ready queue, so live.
        let h = unsafe { hdr.as_ref() };
        check!(
            h.state.get() == FiberState::Ready,
            "resumed fiber {} is not Ready",
            h.id.get()
        );
        h.state.set(FiberState::Running);
        self.current.set(Some(hdr));
        // SAFETY: `h.context` was prepared by `make` or saved by a previous
        // switch out of this fiber; its stack is mapped; we run on the
        // scheduler's stack; the only references held are shared refs to
        // `Cell`-based state (Rule 5).
        unsafe { context::switch(&self.sched_ctx, &h.context) };
        self.current.set(None);
        if h.state.get() == FiberState::Done {
            self.recycle(hdr);
        }
    }

    /// Switch from the current fiber back to the scheduler.
    fn suspend(&self, hdr: Hdr) {
        // SAFETY: `hdr` is the running fiber, so we are on its stack; the
        // scheduler context was saved by `resume`. Only shared refs are live.
        unsafe { context::switch(&hdr.as_ref().context, &self.sched_ctx) };
    }

    /// `Cx::yield_now`: requeue the running fiber and give the thread back.
    pub(crate) fn yield_current(&self) {
        let hdr = self.current();
        // SAFETY: the running fiber is live.
        let h = unsafe { hdr.as_ref() };
        check!(
            h.state.get() == FiberState::Running,
            "yield from non-running fiber {}",
            h.id.get()
        );
        h.state.set(FiberState::Ready);
        // SAFETY: a Running fiber is in no queue.
        unsafe { self.ready.push(hdr) };
        self.suspend(hdr);
    }

    /// Park the running fiber until `unpark(.., kind)`. Rule 3: only the
    /// primitive named by `kind` may wake it.
    #[allow(dead_code)] // first callers land in stage 3 (sync); tested here
    pub(crate) fn park(&self, kind: WaitKind) {
        let hdr = self.current();
        // SAFETY: the running fiber is live.
        let h = unsafe { hdr.as_ref() };
        check!(
            h.state.get() == FiberState::Running,
            "park from non-running fiber {}",
            h.id.get()
        );
        h.wait.set(kind);
        h.state.set(FiberState::Waiting);
        self.suspend(hdr);
    }

    /// Rule 13: wake a fiber parked on `kind`. The fiber must be `Waiting`
    /// on exactly that kind, and `kind` is never `Io` (I/O ends on its CQE).
    #[allow(dead_code)] // first callers land in stage 3 (sync); tested here
    pub(crate) fn unpark(&self, hdr: Hdr, kind: WaitKind) {
        // SAFETY: callers hold headers of fibers owned by this scheduler.
        let h = unsafe { hdr.as_ref() };
        if kind == WaitKind::Io || h.state.get() != FiberState::Waiting || h.wait.get() != kind {
            fatal(format_args!(
                "unpark(fiber {}) as {}, but it is {:?}/{}",
                h.id.get(),
                kind.name(),
                h.state.get(),
                h.wait.get().name()
            ));
        }
        self.push_ready(hdr);
    }

    /// The fiber's last act: mark it `Done` and switch out for good.
    fn finish_current(&self) -> ! {
        let hdr = self.current();
        // SAFETY: the running fiber is live.
        let h = unsafe { hdr.as_ref() };
        check!(
            h.state.get() == FiberState::Running,
            "finishing non-running fiber {}",
            h.id.get()
        );
        h.state.set(FiberState::Done);
        // SAFETY: as in `suspend`. The saved sp is never resumed: a recycled
        // fiber gets a fresh frame from `make`.
        unsafe { context::switch(&h.context, &self.sched_ctx) };
        fatal(format_args!("finished fiber {} was resumed", h.id.get()))
    }

    // ----------------------------------------------------- recycle and free

    /// A fiber that just finished and is switched out: keep it for reuse, up
    /// to `max_idle_fibers`, else give its stack back.
    fn recycle(&self, hdr: Hdr) {
        // SAFETY: live; we are on the scheduler stack so the fiber is not running.
        let h = unsafe { hdr.as_ref() };
        check!(
            h.task.get().is_none(),
            "finished fiber {} still owns a task",
            h.id.get()
        );
        self.live.set(self.live.get() - 1);
        if self.idle_len.get() < self.opts.max_idle_fibers {
            h.next.set(self.idle_head.get());
            self.idle_head.set(Some(hdr));
            self.idle_len.set(self.idle_len.get() + 1);
        } else {
            self.release(hdr);
        }
    }

    /// Remove from the registry and unmap. `hdr` must be Done (or never
    /// started) and in no queue.
    fn release(&self, hdr: Hdr) {
        // SAFETY: live until the unmap below.
        let h = unsafe { hdr.as_ref() };
        let index = h.registry_index.get();
        let stack = h.stack.get();
        let mut registry = self.registry.take();
        dcheck!(
            registry.get(index) == Some(&hdr),
            "registry index of fiber {} is stale",
            h.id.get()
        );
        registry.swap_remove(index);
        if let Some(moved) = registry.get(index) {
            // SAFETY: registered headers are live.
            unsafe { moved.as_ref() }.registry_index.set(index);
        }
        self.registry.set(registry);
        // SAFETY: Done fiber, switched out, unlinked: nothing runs on or
        // points into the stack; `h` is not used again.
        unsafe { stack.unmap() };
    }

    #[cfg(test)]
    pub(crate) fn headers(&self) -> Vec<Hdr> {
        let registry = self.registry.take();
        let copy = registry.clone();
        self.registry.set(registry);
        copy
    }

    // -------------------------------------------------------------- shutdown

    /// Abort, naming each culprit, if any started fiber is still unfinished.
    /// `why` says which check fired.
    pub(crate) fn abort_if_started_live(&self, why: &str) {
        let registry = self.registry.take();
        let mut groups: Vec<(&'static str, Vec<u64>)> = Vec::new();
        for hdr in &registry {
            // SAFETY: registered headers are live.
            let h = unsafe { hdr.as_ref() };
            if h.state.get() == FiberState::Done || h.is_unstarted() {
                continue;
            }
            let name = if h.state.get() == FiberState::Waiting {
                h.wait.get().name()
            } else {
                "ready"
            };
            match groups.iter_mut().find(|(n, _)| *n == name) {
                Some((_, ids)) => ids.push(h.id.get()),
                None => groups.push((name, vec![h.id.get()])),
            }
        }
        self.registry.set(registry);
        if groups.is_empty() {
            return;
        }
        let mut report = String::new();
        for (i, (name, ids)) in groups.iter().enumerate() {
            if i > 0 {
                report.push_str(", ");
            }
            let list: Vec<String> = ids.iter().map(u64::to_string).collect();
            report.push_str(&format!(
                "{} on {} (fiber {})",
                ids.len(),
                name,
                list.join(", ")
            ));
        }
        fatal(format_args!(
            "{why}: fibers still parked, and nothing can wake them: {report}"
        ));
    }

    /// `Io::drop`: free everything. Never runs fibers; aborts if a started
    /// one is unfinished (the seatbelt, section 6 "Shutdown"). Closures of
    /// never-started fibers are dropped without running.
    pub(crate) fn teardown(&self) {
        self.abort_if_started_live("Io dropped");
        self.ready.forget_all();
        self.idle_head.set(None);
        self.idle_len.set(0);
        self.live.set(0);
        let registry = self.registry.take();
        for hdr in registry {
            // SAFETY: live until its unmap, below.
            let h = unsafe { hdr.as_ref() };
            // SAFETY: an unstarted fiber's closure is live and unreachable.
            unsafe { h.drop_unstarted_task() };
            let stack = h.stack.get();
            // SAFETY: the fiber is Done, idle or never started: nothing runs
            // on or points into its stack, and `h` is not used again.
            unsafe { stack.unmap() };
        }
    }
}

/// First code a new fiber runs, on its own stack, reached through the
/// trampoline in `context.rs`.
extern "C" fn fiber_entry(arg: *mut c_void) -> ! {
    // SAFETY: `arg` is the header pointer `spawn` gave to `Context::make`; the
    // header outlives this fiber's final switch (freed only after it).
    let h = unsafe { &*arg.cast::<FiberHeader>() };
    // SAFETY: the scheduler outlives every header it owns.
    let sched = unsafe { &*h.sched.get() };
    {
        // SAFETY: `Sched` is only ever built inside an `Rc` (`Sched::new`), so
        // this pointer came from `Rc::as_ptr` and the count may be bumped.
        let rc = unsafe {
            Rc::increment_strong_count(std::ptr::from_ref(sched));
            Rc::from_raw(std::ptr::from_ref(sched))
        };
        let cx = Cx::new(rc);
        // SAFETY: spawn stored a live closure for this fiber.
        unsafe { h.run_task(&cx) };
        // `cx` (and the closure's captures) drop here, before the last switch:
        // this function never returns, so nothing after it would be dropped.
    }
    sched.finish_current()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::options::Options;

    fn sched() -> Rc<Sched> {
        Sched::new(Options::default().validate().unwrap())
    }

    /// Hand out a leaked detached header; tests free it explicitly.
    fn header() -> Hdr {
        NonNull::from(Box::leak(FiberHeader::detached()))
    }

    fn free(h: Hdr) {
        // SAFETY: came from `header()` and is unqueued by now.
        drop(unsafe { Box::from_raw(h.as_ptr()) });
    }

    fn wait(h: Hdr) {
        // SAFETY: live test header.
        let hr = unsafe { h.as_ref() };
        hr.state.set(FiberState::Waiting);
        hr.wait.set(WaitKind::Mutex);
    }

    // Prediction: three waiting fibers pushed in order a, b, c pop in order a, b, c.
    #[test]
    fn ready_queue_is_fifo() {
        let s = sched();
        let (a, b, c) = (header(), header(), header());
        for h in [a, b, c] {
            wait(h);
            s.push_ready(h);
        }
        assert_eq!(s.ready.pop(), Some(a));
        assert_eq!(s.ready.pop(), Some(b));
        assert_eq!(s.ready.pop(), Some(c));
        assert_eq!(s.ready.pop(), None);
        for h in [a, b, c] {
            free(h);
        }
    }

    // Prediction (invariant 5): waking the same fiber three times queues it once.
    #[test]
    fn double_push_queues_once() {
        let s = sched();
        let a = header();
        wait(a);
        s.push_ready(a);
        s.push_ready(a);
        s.push_ready(a);
        assert_eq!(s.ready.pop(), Some(a));
        assert_eq!(s.ready.pop(), None);
        free(a);
    }

    // Prediction: a popped fiber can be queued again (its link was cleared).
    #[test]
    fn pop_then_requeue() {
        let s = sched();
        let (a, b) = (header(), header());
        wait(a);
        wait(b);
        s.push_ready(a);
        s.push_ready(b);
        assert_eq!(s.ready.pop(), Some(a));
        // SAFETY: live test header.
        unsafe { a.as_ref() }.state.set(FiberState::Waiting);
        s.push_ready(a);
        assert_eq!(s.ready.pop(), Some(b));
        assert_eq!(s.ready.pop(), Some(a));
        assert!(s.ready.is_empty());
        free(a);
        free(b);
    }

    #[test]
    fn quiesced_follows_the_queue() {
        let s = sched();
        assert!(s.quiesced());
        let a = header();
        wait(a);
        s.push_ready(a);
        assert!(!s.quiesced());
        s.ready.pop();
        assert!(s.quiesced());
        free(a);
    }
    // ----- fibers on real stacks (need the context switch; not run under Miri)

    use crate::io::Io;
    use crate::testutil::death_test;

    fn io() -> Io {
        Io::new(Options::default()).unwrap()
    }

    // Prediction: a fiber parked on a Mutex and woken as a Mutex resumes and finishes.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn unpark_with_the_right_kind_resumes() {
        let mut io = io();
        let done = Rc::new(Cell::new(false));
        let d = done.clone();
        io.spawn(move |cx| {
            cx.sched().park(WaitKind::Mutex);
            d.set(true);
        })
        .unwrap();
        io.sched().resume_ready(8); // fiber runs until it parks
        assert!(!done.get());
        let h = io.sched().headers()[0];
        io.sched().unpark(h, WaitKind::Mutex);
        io.run_until_done();
        assert!(done.get());
    }

    // Prediction: quiesce with one fiber parked on a Mutex names Mutex and fiber 1.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_quiesce_names_mutex() {
        death_test(
            "sched::tests::death_quiesce_names_mutex",
            || {
                let mut io = io();
                io.spawn(|cx| cx.sched().park(WaitKind::Mutex)).unwrap();
                io.run_until_done();
            },
            &["quiesced", "1 on Mutex (fiber 1)"],
        );
    }

    // Prediction: two stuck primitives are both named.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_quiesce_names_both_primitives() {
        death_test(
            "sched::tests::death_quiesce_names_both_primitives",
            || {
                let mut io = io();
                io.spawn(|cx| cx.sched().park(WaitKind::Latch)).unwrap();
                io.spawn(|cx| cx.sched().park(WaitKind::BufferPool))
                    .unwrap();
                io.run_until_done();
            },
            &["1 on Latch (fiber 1)", "1 on FixedBufferPool (fiber 2)"],
        );
    }

    // Prediction: dropping an Io with a parked fiber aborts and names it (the seatbelt).
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_drop_with_parked_fiber() {
        death_test(
            "sched::tests::death_drop_with_parked_fiber",
            || {
                let io = io();
                io.spawn(|cx| cx.sched().park(WaitKind::Semaphore)).unwrap();
                io.sched().resume_ready(8);
                drop(io);
            },
            &["Io dropped", "1 on Semaphore (fiber 1)"],
        );
    }

    // Prediction (Rule 13): unparking as the wrong kind aborts, naming both kinds.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_unpark_wrong_kind() {
        death_test(
            "sched::tests::death_unpark_wrong_kind",
            || {
                let io = io();
                io.spawn(|cx| cx.sched().park(WaitKind::Mutex)).unwrap();
                io.sched().resume_ready(8);
                let h = io.sched().headers()[0];
                io.sched().unpark(h, WaitKind::Latch);
            },
            &["unpark(fiber 1) as Latch", "Mutex"],
        );
    }

    // Prediction (Rule 3 / 13): an I/O wait can never be ended by unpark.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_unpark_io_wait() {
        death_test(
            "sched::tests::death_unpark_io_wait",
            || {
                let io = io();
                io.spawn(|cx| cx.sched().park(WaitKind::Io)).unwrap();
                io.sched().resume_ready(8);
                let h = io.sched().headers()[0];
                io.sched().unpark(h, WaitKind::Io);
            },
            &["unpark(fiber 1) as I/O"],
        );
    }

    // Prediction (invariant 5): waking a finished fiber aborts.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_push_ready_on_done_fiber() {
        death_test(
            "sched::tests::death_push_ready_on_done_fiber",
            || {
                let mut io = io();
                io.spawn(|_| {}).unwrap();
                io.run_until_done();
                let h = io.sched().headers()[0];
                io.sched().push_ready(h);
            },
            &["push_ready on finished fiber"],
        );
    }
}
