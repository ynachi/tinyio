//! The fiber header and the type-erased task it runs.
//!
//! One fiber is one mapping (see `stack.rs`). At its very top, 64-byte
//! aligned, sits the `FiberHeader`; just below it the spawned closure; below
//! that the initial context frame and the stack proper:
//!
//! ```text
//!   high   +-------------------+ <- Stack::high
//!          | FiberHeader       |
//!          +-------------------+
//!          | closure F         |
//!          +-------------------+ <- stack_top handed to Context::make
//!          | initial frame     |
//!          | ... stack grows ..|
//!   low    +-------------------+ <- Stack::low
//!          | guard (PROT_NONE) |
//! ```
//!
//! The header stays valid until the stack is unmapped, which happens only
//! when its fiber is `Done` and switched out, or was never started.

use std::cell::Cell;
use std::fmt;
use std::ptr::NonNull;

use super::context::Context;
use super::stack::Stack;
use crate::cx::Cx;
use crate::sched::Sched;
use crate::timer::TimerId;

/// A unique, monotonically increasing fiber identifier (a recycled stack gets
/// a fresh one for every execution).
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct FiberId(pub(crate) u64);

impl FiberId {
    /// The raw number, for logging.
    pub fn get(self) -> u64 {
        self.0
    }
}

impl fmt::Display for FiberId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "fiber {}", self.0)
    }
}

/// `Ready` means exactly "in the ready queue" (invariant 5).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum FiberState {
    Ready,
    Running,
    Waiting,
    /// Finished. Also the state of an idle (recycled) fiber.
    Done,
}

/// Why a fiber is `Waiting`. It costs nothing and names the culprit when
/// shutdown aborts with fibers still parked (invariant 10).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[allow(dead_code)] // every kind but `None` is first used from stage 2/3
pub(crate) enum WaitKind {
    None,
    Io,
    Mutex,
    Semaphore,
    Latch,
    Baton,
    Scope,
    BufferPool,
}

impl WaitKind {
    pub(crate) fn name(self) -> &'static str {
        match self {
            WaitKind::None => "none",
            WaitKind::Io => "I/O",
            WaitKind::Mutex => "Mutex",
            WaitKind::Semaphore => "Semaphore",
            WaitKind::Latch => "Latch",
            WaitKind::Baton => "Baton",
            WaitKind::Scope => "Scope",
            WaitKind::BufferPool => "FixedBufferPool",
        }
    }
}

/// How to run and how to drop a closure of a type the header has forgotten.
#[derive(Clone, Copy)]
pub(crate) struct TaskOps {
    /// Move the closure out of `*p` and call it.
    run: unsafe fn(*mut u8, &Cx),
    /// Drop the closure in place without running it.
    drop: unsafe fn(*mut u8),
}

impl TaskOps {
    pub(crate) fn of<F: FnOnce(&Cx)>() -> TaskOps {
        TaskOps {
            run: run_task::<F>,
            drop: drop_task::<F>,
        }
    }
}

/// # Safety
/// `p` points to a live, properly aligned `F` that nothing uses afterwards.
unsafe fn run_task<F: FnOnce(&Cx)>(p: *mut u8, cx: &Cx) {
    // SAFETY: fn contract: `p` is a live `F`, consumed exactly once here.
    let f = unsafe { p.cast::<F>().read() };
    f(cx);
}

/// # Safety
/// `p` points to a live, properly aligned `F` that nothing uses afterwards.
unsafe fn drop_task<F>(p: *mut u8) {
    // SAFETY: fn contract.
    unsafe { p.cast::<F>().drop_in_place() };
}

/// Everything the scheduler knows about one fiber. All fields are `Cell`s: a
/// header is reached through shared pointers from the ready queue, the
/// registry and the fiber's own stack, and no `&mut` to it ever exists
/// (Rule 5).
#[repr(C, align(64))]
pub(crate) struct FiberHeader {
    /// Saved stack pointer while the fiber is suspended.
    pub(crate) context: Context,
    /// The owning scheduler. Valid for the header's whole life: the
    /// scheduler frees headers, never the other way round.
    pub(crate) sched: Cell<*const Sched>,
    pub(crate) state: Cell<FiberState>,
    pub(crate) wait: Cell<WaitKind>,
    /// Link of the ready queue or the idle list. A fiber is in at most one of
    /// them (a queued fiber is `Ready`, an idle one is `Done`).
    pub(crate) next: Cell<Option<NonNull<FiberHeader>>>,
    pub(crate) id: Cell<u64>,
    /// Position in the scheduler's registry (swap-and-pop removal).
    pub(crate) registry_index: Cell<usize>,
    /// Set from spawn until the fiber starts (or is dropped unstarted).
    pub(crate) task: Cell<Option<TaskOps>>,
    /// Where the closure sits on this fiber's stack.
    pub(crate) task_ptr: Cell<*mut u8>,
    pub(crate) stack: Cell<Stack>,
    /// The wheel entry of the op in flight, if it has a timeout. A fiber never
    /// finishes with this set.
    pub(crate) timer: Cell<Option<TimerId>>,
    /// Set when the wheel expired the op and a cancel was queued, so the
    /// `-ECANCELED` that follows is reported as `ETIMEDOUT`.
    pub(crate) timed_out: Cell<bool>,
    /// The `user_data` of the op in flight: what a timeout cancel targets.
    pub(crate) op_user_data: Cell<u64>,
    /// Set when shutdown woke this fiber out of a sync-primitive wait, so the
    /// wait reports `ECANCELED` instead of success.
    pub(crate) wait_cancelled: Cell<bool>,
}

impl FiberHeader {
    pub(crate) fn new(sched: *const Sched, stack: Stack) -> FiberHeader {
        FiberHeader {
            context: Context::empty(),
            sched: Cell::new(sched),
            state: Cell::new(FiberState::Done),
            wait: Cell::new(WaitKind::None),
            next: Cell::new(None),
            id: Cell::new(0),
            registry_index: Cell::new(0),
            task: Cell::new(None),
            task_ptr: Cell::new(std::ptr::null_mut()),
            stack: Cell::new(stack),
            timer: Cell::new(None),
            timed_out: Cell::new(false),
            op_user_data: Cell::new(0),
            wait_cancelled: Cell::new(false),
        }
    }

    /// A fiber that was spawned but has not run yet.
    pub(crate) fn is_unstarted(&self) -> bool {
        self.task.get().is_some()
    }

    /// Run the closure stored at `task_ptr`. Called once, on the fiber's own
    /// stack, by `fiber_entry`.
    ///
    /// # Safety
    /// The header's `task`/`task_ptr` describe a live closure (set by spawn).
    pub(crate) unsafe fn run_task(&self, cx: &Cx) {
        let Some(ops) = self.task.take() else {
            crate::check::fatal(format_args!("fiber {} started twice", self.id.get()));
        };
        // SAFETY: spawn wrote an `F` at `task_ptr` matching `ops`; `take()`
        // above guarantees it is consumed only once.
        unsafe { (ops.run)(self.task_ptr.get(), cx) };
    }

    /// Drop a never-started closure without running it.
    ///
    /// # Safety
    /// Same as [`run_task`](Self::run_task); the fiber must not be started.
    pub(crate) unsafe fn drop_unstarted_task(&self) {
        if let Some(ops) = self.task.take() {
            // SAFETY: the closure is live (task was Some) and now unreachable.
            unsafe { (ops.drop)(self.task_ptr.get()) };
        }
    }

    #[cfg(test)]
    pub(crate) fn detached() -> Box<FiberHeader> {
        Box::new(FiberHeader::new(std::ptr::null(), Stack::none()))
    }
}
