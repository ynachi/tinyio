use std::cell::Cell;
use std::io;
use std::marker::PhantomData;
use std::rc::Rc;

use super::waiters::WaitList;
use crate::check::check;
use crate::cx::{Cx, NotSend};
use crate::detail::fiber::{FiberId, WaitKind};
use crate::sched::Sched;

struct ScopeData {
    sched: Rc<Sched>,
    active: Cell<usize>,
    waiters: WaitList,
}

impl ScopeData {
    fn child_done(&self) {
        let left = self.active.get() - 1;
        self.active.set(left);
        if left == 0 {
            // Last touch of `self`: the parent may pop this frame once it runs.
            self.waiters.wake_all(&self.sched);
        }
    }
}

/// Aborts if the scope frame is left (by a panic caught further up) while a
/// child still borrows it. Unwinding through a live borrow would be UB.
struct JoinGuard<'a>(&'a ScopeData);

impl Drop for JoinGuard<'_> {
    fn drop(&mut self) {
        check!(
            self.0.active.get() == 0,
            "tinyio::scope left with {} child fiber(s) still running; they borrow the frames being unwound",
            self.0.active.get()
        );
    }
}

/// Fibers spawned through this may borrow from the enclosing frame; `scope`
/// does not return until every one of them has finished.
pub struct Scope<'scope, 'env: 'scope> {
    data: &'scope ScopeData,
    // Invariant in both lifetimes, exactly as in `std::thread::Scope`: with
    // covariant markers a child could borrow a local that dies before the join.
    _scope: PhantomData<&'scope mut &'scope ()>,
    _env: PhantomData<&'env mut &'env ()>,
    _p: NotSend,
}

impl<'scope> Scope<'scope, '_> {
    /// Start a child fiber that may borrow anything that outlives the scope.
    /// It runs when the current fiber next suspends. `ECANCELED` once the
    /// runtime is shutting down.
    pub fn spawn<F>(&'scope self, f: F) -> io::Result<FiberId>
    where
        F: FnOnce(&Cx) + 'scope,
    {
        let data = self.data;
        let child = move |cx: &Cx| {
            f(cx); // consumes `f`, so its captures are gone before the parent can wake
            data.child_done();
        };
        data.active.set(data.active.get() + 1);
        // SAFETY: `scope` joins every child before returning, and aborts
        // rather than unwind past a live one (`JoinGuard`), so everything
        // `f` and `data` borrow outlives the fiber's run.
        let spawned = unsafe { data.sched.spawn_raw(child) };
        if spawned.is_err() {
            data.active.set(data.active.get() - 1);
        }
        spawned
    }
}

/// Run `f`, then wait for every fiber it spawned through the [`Scope`].
///
/// A child cannot borrow a local declared inside `f`:
///
/// ```compile_fail,E0373
/// let mut io = tinyio::Io::new(Default::default()).unwrap();
/// io.spawn(|cx| {
///     tinyio::scope(cx, |s| {
///         let local = 5;
///         s.spawn(|_| assert_eq!(local, 5)).ok(); // `local` dies before the join
///     });
/// }).unwrap();
/// io.run_until_done();
/// ```
/// ```
/// let mut io = tinyio::Io::new(Default::default()).unwrap();
/// io.spawn(|cx| {
///     let outer = 5; // the twin: declared outside the scope
///     tinyio::scope(cx, |s| {
///         s.spawn(|_| assert_eq!(outer, 5)).ok();
///     });
/// }).unwrap();
/// io.run_until_done();
/// ```
pub fn scope<'env, F, R>(cx: &Cx, f: F) -> R
where
    F: for<'scope> FnOnce(&'scope Scope<'scope, 'env>) -> R,
{
    let data = ScopeData {
        sched: Rc::clone(cx.sched()),
        active: Cell::new(0),
        waiters: WaitList::new(WaitKind::Scope),
    };
    let _guard = JoinGuard(&data);
    let scope = Scope {
        data: &data,
        _scope: PhantomData,
        _env: PhantomData,
        _p: PhantomData,
    };
    let result = f(&scope);
    while data.active.get() > 0 {
        data.waiters.park(&data.sched);
    }
    result
}
