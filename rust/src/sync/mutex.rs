use std::cell::{Cell, UnsafeCell};
use std::io;
use std::ops::{Deref, DerefMut};
use std::rc::Rc;

use super::waiters::WaitList;
use super::{Attach, bind, check_same};
use crate::cx::Cx;
use crate::detail::fiber::WaitKind;
use crate::sched::Sched;

/// A mutex that owns its data, for fibers on one thread. Holding the guard
/// across a suspend (I/O, `yield_now`) is fine: other fibers wait.
///
/// The guard cannot outlive the mutex, and the mutex cannot leave its thread:
///
/// ```compile_fail,E0597
/// let io = tinyio::Io::new(Default::default()).unwrap();
/// let mut io = io;
/// io.spawn(|cx| {
///     let g = {
///         let m = tinyio::Mutex::new(cx, 1u8);
///         m.lock(cx).unwrap() // guard escapes the block that owns `m`
///     };
///     drop(g);
/// }).unwrap();
/// io.run_until_done();
/// ```
/// ```
/// let mut io = tinyio::Io::new(Default::default()).unwrap();
/// io.spawn(|cx| {
///     let m = tinyio::Mutex::new(cx, 1u8);
///     let g = m.lock(cx).unwrap();
///     drop(g); // the twin: guard dropped inside the block
/// }).unwrap();
/// io.run_until_done();
/// ```
///
/// ```compile_fail,E0277
/// fn needs_send<T: Send>() {}
/// needs_send::<tinyio::Mutex<u8>>();
/// ```
pub struct Mutex<T> {
    sched: Rc<Sched>,
    held: Cell<bool>,
    waiters: WaitList,
    data: UnsafeCell<T>,
}

impl<T> Mutex<T> {
    #[inline]
    pub fn new(at: &impl Attach, value: T) -> Mutex<T> {
        Mutex {
            sched: bind(at),
            held: Cell::new(false),
            waiters: WaitList::new(WaitKind::Mutex),
            data: UnsafeCell::new(value),
        }
    }

    /// Wait for the lock, in FIFO order. `ECANCELED` if the runtime is stopping
    /// and the lock was not free.
    #[inline]
    pub fn lock<'a>(&'a self, cx: &Cx) -> io::Result<MutexGuard<'a, T>> {
        check_same(&self.sched, cx, "Mutex");
        if self.held.get() {
            // `unlock` hands ownership over before waking us, so on return the lock is ours.
            self.waiters.wait(&self.sched)?;
        } else {
            self.held.set(true);
        }
        Ok(MutexGuard { mutex: self })
    }

    /// Take the lock if it is free.
    #[inline]
    pub fn try_lock(&self) -> Option<MutexGuard<'_, T>> {
        if self.held.replace(true) {
            return None;
        }
        Some(MutexGuard { mutex: self })
    }

    /// The value, by exclusive access: no lock needed, nobody else can have it.
    pub fn get_mut(&mut self) -> &mut T {
        self.data.get_mut()
    }

    /// Consume the mutex and return the value. Aborts if a fiber is still
    /// waiting (it borrows the mutex, so safe code cannot get here).
    pub fn into_inner(self) -> T {
        self.data.into_inner()
    }

    #[inline]
    fn unlock(&self) {
        // With a waiter, `held` stays true: the lock moves straight to it, so a
        // fiber that runs first cannot barge in.
        if !self.waiters.wake_one(&self.sched) {
            self.held.set(false);
        }
    }
}

/// Releases the lock on drop. Owned by the guard, not by a fiber, so another
/// fiber on the same thread may drop it.
#[must_use = "dropping the guard unlocks at once"]
pub struct MutexGuard<'a, T> {
    mutex: &'a Mutex<T>,
}

impl<T> Deref for MutexGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &T {
        // SAFETY: a live guard means the lock is held (invariant: one guard per
        // mutex), so no other reference to the data exists.
        unsafe { &*self.mutex.data.get() }
    }
}

impl<T> DerefMut for MutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: as in `deref`, and `&mut self` makes this the only borrow of the guard.
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<T> Drop for MutexGuard<'_, T> {
    #[inline]
    fn drop(&mut self) {
        self.mutex.unlock();
    }
}
