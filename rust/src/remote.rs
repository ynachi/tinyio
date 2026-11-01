#![forbid(unsafe_code)]
//! `Remote`, the only handle other threads hold, and the state it shares with
//! the loop. Everything here is safe code: the queue is `ArrayQueue`, and the
//! `Arc` owns the eventfd, so a late `stop` can never write to a closed or
//! recycled descriptor (the C++ needs a mutex for that).

use std::error::Error;
use std::fmt;
use std::fs::File;
use std::io::{ErrorKind, Write};
use std::os::fd::{AsRawFd, OwnedFd, RawFd};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering, fence};

use crossbeam_queue::ArrayQueue;

use crate::cx::Cx;

pub(crate) type Task = Box<dyn FnOnce(&Cx) + Send>;

/// Top bit of `post_state`: the loop has closed its queue.
const CLOSED: u64 = 1 << 63;

/// Why `Remote::post` refused a closure (which is dropped).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PostError {
    /// The queue is at `Options::post_queue_capacity`.
    Full,
    /// The runtime is shutting down or gone.
    Closed,
}

impl fmt::Display for PostError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            PostError::Full => "post queue is full",
            PostError::Closed => "runtime is closed",
        })
    }
}

impl Error for PostError {}

pub(crate) struct Shared {
    queue: ArrayQueue<Task>,
    /// Low 63 bits: posts counted but not yet popped. Top bit: closed. It is
    /// only ever added to and subtracted from, never reset: a producer that
    /// lost the race to a full queue subtracts its own +1 later.
    post_state: AtomicU64,
    stop: AtomicBool,
    /// The owner is about to block (or is blocked) in the kernel, so a post
    /// must write the eventfd. Not set while the loop is running: then the
    /// next tick finds the post without a syscall.
    sleeping: AtomicBool,
    wake: File,
    /// Where the kernel writes the eventfd counter (Rule 12: it must not move,
    /// so it lives here and not in `Io`).
    wake_slot: AtomicU64,
    /// Test hook: how many times `wake` wrote the eventfd.
    #[cfg(test)]
    wakes: std::sync::atomic::AtomicUsize,
}

impl Shared {
    pub(crate) fn new(eventfd: OwnedFd, capacity: usize) -> Shared {
        Shared {
            queue: ArrayQueue::new(capacity),
            post_state: AtomicU64::new(0),
            stop: AtomicBool::new(false),
            sleeping: AtomicBool::new(false),
            wake: File::from(eventfd),
            wake_slot: AtomicU64::new(0),
            #[cfg(test)]
            wakes: std::sync::atomic::AtomicUsize::new(0),
        }
    }

    pub(crate) fn post(&self, task: Task) -> Result<(), PostError> {
        let mut cur = self.post_state.load(Ordering::Relaxed);
        loop {
            if cur & CLOSED != 0 {
                return Err(PostError::Closed);
            }
            match self.post_state.compare_exchange_weak(
                cur,
                cur + 1,
                Ordering::AcqRel,
                Ordering::Relaxed,
            ) {
                Ok(_) => break,
                Err(seen) => cur = seen,
            }
        }
        if self.queue.push(task).is_err() {
            self.post_state.fetch_sub(1, Ordering::AcqRel);
            return Err(PostError::Full);
        }
        // Pairs with the fence in `announce_sleep`: either this sees the flag
        // or the owner's re-check sees our count. Only the producer whose swap
        // takes the flag down writes the eventfd, so a burst into a sleeping
        // loop costs one write, not one per post. The plain load stays in front
        // so a post to a running loop is one load, not a read-modify-write on a
        // line the owner also writes.
        //
        // No wakeup is lost: a producer that sees the flag up but loses the swap
        // lost it either to another producer (who writes) or to the owner's own
        // `clear_sleep` (the owner is awake). Either way the owner's next
        // `announce_sleep` fence pairs with ours, so it finds our count in
        // `pending` before it can block again. If our push is not visible yet,
        // `pending() > 0` keeps the loop ticking. A producer that clears the
        // flag while the owner is deciding not to block (`announce_sleep` true)
        // writes a wake the owner did not need: one spurious eventfd completion.
        fence(Ordering::SeqCst);
        if self.sleeping.load(Ordering::Relaxed) && self.sleeping.swap(false, Ordering::SeqCst) {
            self.wake();
        }
        Ok(())
    }

    /// Owner side, just before blocking: publish the sleep, then re-check.
    /// `true` means do not block: a post or a stop is already visible. Either
    /// way call `clear_sleep` afterwards.
    pub(crate) fn announce_sleep(&self) -> bool {
        self.sleeping.store(true, Ordering::Relaxed);
        fence(Ordering::SeqCst);
        self.pending() > 0 || self.stop_requested()
    }

    pub(crate) fn clear_sleep(&self) {
        self.sleeping.store(false, Ordering::Relaxed);
    }

    /// Owner side. A producer between its count and its push shows as
    /// `pending() > 0` with an empty queue; the loop must keep ticking.
    pub(crate) fn pop(&self) -> Option<Task> {
        let task = self.queue.pop()?;
        self.post_state.fetch_sub(1, Ordering::AcqRel);
        Some(task)
    }

    pub(crate) fn pending(&self) -> u64 {
        self.post_state.load(Ordering::Acquire) & !CLOSED
    }

    /// After this no `post` succeeds (invariant 13).
    pub(crate) fn close(&self) {
        self.post_state.fetch_or(CLOSED, Ordering::AcqRel);
    }

    /// Drop every queued closure without running it.
    pub(crate) fn discard(&self) {
        while self.pop().is_some() {}
    }

    pub(crate) fn request_stop(&self) {
        self.stop.store(true, Ordering::Release);
        self.wake();
    }

    pub(crate) fn stop_requested(&self) -> bool {
        self.stop.load(Ordering::Acquire)
    }

    /// `WouldBlock` means the counter is saturated, which already wakes the reader.
    fn wake(&self) {
        #[cfg(test)]
        self.wakes.fetch_add(1, Ordering::Relaxed);
        match (&self.wake).write(&1u64.to_ne_bytes()) {
            Ok(_) => {}
            Err(e) if e.kind() == ErrorKind::WouldBlock => {}
            Err(e) => crate::check::error(format_args!("eventfd wake failed: {e}")),
        }
    }

    pub(crate) fn eventfd(&self) -> RawFd {
        self.wake.as_raw_fd()
    }

    pub(crate) fn wake_slot(&self) -> *mut u64 {
        self.wake_slot.as_ptr()
    }
}

/// The only `Send + Sync` handle to a runtime. Cheap to clone.
#[derive(Clone)]
pub struct Remote {
    shared: Arc<Shared>,
}

impl std::fmt::Debug for Remote {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Remote")
            .field("stop_requested", &self.shared.stop_requested())
            .finish_non_exhaustive()
    }
}

impl Remote {
    pub(crate) fn new(shared: Arc<Shared>) -> Remote {
        Remote { shared }
    }

    /// Run `f` as a fiber on the runtime's thread. On `Err` the closure is
    /// dropped unrun. `Ok` means queued, not run: the closure is dropped unrun
    /// if the runtime shuts down before the loop gets to it, and also, with an
    /// error logged, if the process is so short of memory that no fiber stack
    /// can be mapped and no running fiber is left to give one back. That is
    /// out-of-memory, not a protocol failure, so it is a logged drop and not an
    /// abort. A caller that needs to know a closure ran should make it report back.
    ///
    /// The closure must be `Send`:
    ///
    /// ```compile_fail,E0277
    /// let io = tinyio::Io::new(Default::default()).unwrap();
    /// let rc = std::rc::Rc::new(1);
    /// io.remote().post(move |_| drop(rc)).ok();
    /// ```
    /// ```
    /// let io = tinyio::Io::new(Default::default()).unwrap();
    /// let n = std::sync::Arc::new(1); // the twin: Arc is Send
    /// io.remote().post(move |_| drop(n)).ok();
    /// ```
    pub fn post<F>(&self, f: F) -> Result<(), PostError>
    where
        F: FnOnce(&Cx) + Send + 'static,
    {
        self.shared.post(Box::new(f))
    }

    /// Ask the loop to stop: `Io::run` then cancels what is in flight, drains,
    /// and returns. Sticky, and safe to call at any time from any thread.
    pub fn stop(&self) {
        self.shared.request_stop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn shared(capacity: usize) -> Shared {
        Shared::new(crate::ring::new_eventfd().unwrap(), capacity)
    }

    fn noop() -> Task {
        Box::new(|_| {})
    }

    // Prediction: a refused (Full) post gives its count back; the queued one keeps its own.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn full_does_not_leak_a_count() {
        let s = shared(1);
        assert_eq!(s.post(noop()), Ok(()));
        assert_eq!(s.post(noop()), Err(PostError::Full));
        assert_eq!(s.pending(), 1);
        assert!(s.pop().is_some());
        assert_eq!(s.pending(), 0);
    }

    // Prediction: a burst of posts that arrives while the owner sleeps costs ONE
    // eventfd write, not one per post. The owner announces the sleep (the queue
    // is empty, so it would block) and does not consume; 4 threads post 1,000
    // each; the write counter is exactly 1. Afterwards the owner clears the flag,
    // drains, and every task is there.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn a_burst_while_asleep_writes_the_eventfd_once() {
        let s = Arc::new(shared(4096));
        assert!(!s.announce_sleep(), "empty queue: the owner would block");
        let producers: Vec<_> = (0..4)
            .map(|_| {
                let s = s.clone();
                std::thread::spawn(move || {
                    for _ in 0..1000 {
                        s.post(noop()).unwrap();
                    }
                })
            })
            .collect();
        for p in producers {
            p.join().unwrap();
        }
        assert_eq!(s.wakes.load(Ordering::Relaxed), 1);
        s.clear_sleep();
        let mut popped = 0;
        while s.pop().is_some() {
            popped += 1;
        }
        assert_eq!(popped, 4000);
        assert_eq!(s.pending(), 0);
    }

    // Prediction: a busy owner (flag down) costs the producers no eventfd write at all.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn posts_to_a_running_owner_write_nothing() {
        let s = shared(64);
        for _ in 0..50 {
            s.post(noop()).unwrap();
        }
        assert_eq!(s.wakes.load(Ordering::Relaxed), 0);
    }

    // Prediction: discarding after the close empties the queue and zeroes the
    // count but keeps the closed bit, so a late post is still refused.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn discard_keeps_the_queue_closed() {
        let s = shared(4);
        assert_eq!(s.post(noop()), Ok(()));
        s.close();
        assert_eq!(s.post(noop()), Err(PostError::Closed));
        s.discard();
        assert_eq!(s.pending(), 0);
        assert_eq!(s.post(noop()), Err(PostError::Closed));
    }
}
