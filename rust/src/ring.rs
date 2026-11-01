//! The io_uring wrapper: SQE submission with retry, CQE reaping, `UserData`,
//! and the in-flight count (invariants 2, 3, 4).
//!
//! The ring sits in an `UnsafeCell` and every method takes `&self` (Rule 5:
//! no `RefCell`). The one accessor, [`Ring::with`], hands a closure a
//! `&mut IoUring` for the length of that closure and nothing else. No method
//! here calls back into scheduler code while holding that borrow: `reap` copies
//! CQEs into a stack batch first and processes them after the borrow ended, and
//! the retry loops invoke their `reap` callback between attempts.

use std::cell::{Cell, UnsafeCell};
use std::io;
use std::mem::MaybeUninit;
use std::os::fd::{FromRawFd, OwnedFd};
use std::ptr::{self, NonNull};
use std::time::Duration;

use io_uring::squeue::{Entry, PushError};
use io_uring::types::{SubmitArgs, Timespec};
use io_uring::{EnterFlags, IoUring, cqueue};

use crate::check::{check, fatal};
use crate::detail::fiber::FiberHeader;

/// Marks a `Completion` whose CQE has not arrived. The kernel never returns it.
const PENDING: i32 = i32::MIN;

/// What a fiber waits on, living in the frame of its blocked `syscall` call.
/// Its address is the `user_data` (invariants 1 and 2): one op per fiber, and
/// the fiber stays parked, so this frame stays alive, until the CQE arrives.
#[repr(align(8))]
pub(crate) struct Completion {
    fiber: NonNull<FiberHeader>,
    result: Cell<i32>,
}

impl Completion {
    pub(crate) fn new(fiber: NonNull<FiberHeader>) -> Completion {
        Completion {
            fiber,
            result: Cell::new(PENDING),
        }
    }

    /// The kernel's result. Aborts if called before the CQE arrived.
    pub(crate) fn result(&self) -> i32 {
        let r = self.result.get();
        check!(
            r != PENDING,
            "fiber resumed before its completion arrived (Rule 3)"
        );
        r
    }
}

// The low two bits of an aligned pointer carry the tag (Rule 6).
const _: () = assert!(align_of::<Completion>() >= 4);

const TAG_MASK: u64 = 0b11;
const TAG_FIBER: u64 = 0;
const WAKE: u64 = 1;
const CANCEL: u64 = 2;

/// The three meanings of a `user_data` (invariant 4). `encode` and `decode`
/// are the only code that reads or writes the raw `u64`.
#[derive(Clone, Copy, Debug)]
pub(crate) enum UserData {
    /// An op submitted by a fiber: its blocked `Completion`.
    Fiber(NonNull<Completion>),
    /// The eventfd read (stage 4). Never counted in flight.
    Wake,
    /// A cancel SQE. Counted, and carries no fiber.
    Cancel,
}

impl UserData {
    pub(crate) fn encode(self) -> u64 {
        match self {
            UserData::Fiber(p) => p.as_ptr().expose_provenance() as u64,
            UserData::Wake => WAKE,
            UserData::Cancel => CANCEL,
        }
    }

    pub(crate) fn decode(raw: u64) -> UserData {
        match (raw & TAG_MASK, raw) {
            (TAG_FIBER, _) => {
                let p = ptr::with_exposed_provenance_mut::<Completion>(raw as usize);
                match NonNull::new(p) {
                    Some(p) => UserData::Fiber(p),
                    None => fatal(format_args!("CQE with null user_data")),
                }
            }
            (_, WAKE) => UserData::Wake,
            (_, CANCEL) => UserData::Cancel,
            _ => fatal(format_args!("CQE with corrupt user_data {raw:#x}")),
        }
    }
}

pub(crate) struct Ring {
    ring: UnsafeCell<Option<IoUring>>,
    /// Invariant 3: submissions that still owe a terminal CQE. +1 in `push`
    /// (the one place), -1 in `reap`. The wake read is never counted.
    inflight: Cell<usize>,
    /// The wake read is armed in the kernel (queued, or submitted and not yet
    /// completed). It owes a CQE but is not in `inflight`.
    wake_inflight: Cell<bool>,
}

/// A non-blocking, close-on-exec eventfd for `Remote` to wake the loop with.
pub(crate) fn new_eventfd() -> io::Result<OwnedFd> {
    // SAFETY: eventfd takes no pointers; the result is checked.
    let fd = unsafe { libc::eventfd(0, libc::EFD_CLOEXEC | libc::EFD_NONBLOCK) };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: eventfd returned a fresh descriptor that nothing else owns.
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

impl Ring {
    pub(crate) const fn closed() -> Ring {
        Ring {
            ring: UnsafeCell::new(None),
            inflight: Cell::new(0),
            wake_inflight: Cell::new(false),
        }
    }

    /// Create the ring: `SINGLE_ISSUER | DEFER_TASKRUN | TASKRUN_FLAG`, created
    /// disabled and enabled at once. `Io` is `!Send`, so the creating thread is
    /// the one that runs the loop, which is what `SINGLE_ISSUER` binds to.
    /// The CQ is twice the SQ. Kernel floor: 6.1, no fallback.
    pub(crate) fn open(&self, entries: u32) -> io::Result<()> {
        let ring = IoUring::<Entry, cqueue::Entry>::builder()
            .setup_single_issuer()
            .setup_defer_taskrun()
            .setup_taskrun_flag()
            .setup_r_disabled()
            .setup_cqsize(entries.saturating_mul(2))
            .setup_clamp()
            .build(entries)?;
        ring.submitter().register_enable_rings()?;
        // SAFETY: single thread; no `with` borrow is active (we are in `open`).
        let slot = unsafe { &mut *self.ring.get() };
        check!(slot.is_none(), "ring opened twice");
        *slot = Some(ring);
        Ok(())
    }

    /// Run `f` with the ring. `f` must not call back into anything that
    /// touches this `Ring` (all callers are in this file and do not).
    fn with<R>(&self, f: impl FnOnce(&mut IoUring) -> R) -> R {
        // SAFETY: the runtime is single-threaded (Rules 1, 2) and `f` is a
        // short closure from this file that never re-enters `Ring`, so this
        // is the only live reference to the `IoUring`.
        let slot = unsafe { &mut *self.ring.get() };
        match slot {
            Some(ring) => f(ring),
            None => fatal(format_args!("the ring is not open")),
        }
    }

    pub(crate) fn inflight(&self) -> usize {
        self.inflight.get()
    }

    pub(crate) fn wake_inflight(&self) -> bool {
        self.wake_inflight.get()
    }

    /// Are completions already in the CQ?
    pub(crate) fn cq_ready(&self) -> bool {
        self.with(|r| !r.completion().is_empty())
    }

    /// Queue one SQE and, unless it is the wake read, count it in flight
    /// (invariant 3: the only place a submission is counted). The entry's
    /// `user_data` is set from `ud`. `Err` means the SQ is full and nothing
    /// was queued or counted.
    ///
    /// # Safety
    /// Every pointer in `entry` (buffers, paths, timespecs) must stay valid
    /// until this submission's CQE has been reaped. For a fiber's op that is
    /// the blocked `syscall` frame (invariant 2).
    pub(crate) unsafe fn push(&self, entry: &Entry, ud: UserData) -> Result<(), PushError> {
        let mut entry = entry.clone();
        entry.set_user_data(ud.encode());
        // SAFETY: forwarded from this fn's contract.
        self.with(|r| unsafe { r.submission().push(&entry) })?;
        if matches!(ud, UserData::Wake) {
            self.wake_inflight.set(true);
        } else {
            self.inflight.set(self.inflight.get() + 1);
        }
        Ok(())
    }

    /// Hand queued SQEs to the kernel, up to four attempts: `EINTR` retries;
    /// `EBUSY`/`EAGAIN` mean the CQ could not be flushed, so `reap` once (it
    /// only marks fibers ready) and retry.
    ///
    /// With `get_events` the enter always carries `GETEVENTS` and waits for
    /// nothing (`min_complete = 0`), like liburing's
    /// `io_uring_submit_and_get_events`. `DEFER_TASKRUN` only runs completion
    /// work inside such an enter, and relying on the crate's own `submit()` to
    /// add the flag misses the case where the `TASKRUN` flag is not yet visible
    /// (AGENTS.md "Known knobs"). The obvious safe alternative, waiting for one
    /// CQE with a zero timeout, is a real sleep/wake cycle in the kernel and
    /// costs ~22 us per call against ~0.12 us (see docs/deviations.md).
    pub(crate) fn submit(&self, get_events: bool, mut reap: impl FnMut()) -> io::Result<()> {
        let mut reaped = false;
        for _ in 0..4 {
            let r = self.with(|ring| {
                if get_events {
                    // The SQ was synced when the `SubmissionQueue` view was
                    // dropped, so `len()` is exactly the number of queued SQEs.
                    let queued = ring.submission().len() as u32;
                    // SAFETY: `enter` with no extra argument (`None`), nothing
                    // to wait for, and `to_submit` equal to the number of valid
                    // SQEs in the SQ. Their pointers are valid per `push`'s
                    // contract (invariant 2).
                    unsafe {
                        ring.submitter().enter::<libc::sigset_t>(
                            queued,
                            0,
                            EnterFlags::GETEVENTS.bits(),
                            None,
                        )
                    }
                } else {
                    ring.submitter().submit()
                }
            });
            let Err(e) = r else { return Ok(()) };
            match e.raw_os_error() {
                Some(libc::EINTR) => {}
                Some(libc::EBUSY | libc::EAGAIN) if !reaped => {
                    reaped = true;
                    reap();
                }
                _ => return Err(e),
            }
        }
        Err(io::Error::from_raw_os_error(libc::EAGAIN))
    }

    /// Submit and sleep until one CQE arrives, the eventfd is written (stage 4)
    /// or `timeout` passes. Only call when nothing is runnable (invariant 8).
    /// `EINTR` and `ETIME` are success; `EBUSY`/`EAGAIN` reap and succeed.
    pub(crate) fn wait(&self, timeout: Option<Duration>, mut reap: impl FnMut()) -> io::Result<()> {
        let r = self.with(|ring| match timeout {
            None => ring.submitter().submit_and_wait(1),
            Some(d) => {
                let ts = Timespec::from(d);
                ring.submitter()
                    .submit_with_args(1, &SubmitArgs::new().timespec(&ts))
            }
        });
        let Err(e) = r else { return Ok(()) };
        match e.raw_os_error() {
            Some(libc::EINTR | libc::ETIME) => Ok(()),
            Some(libc::EBUSY | libc::EAGAIN) => {
                reap();
                Ok(())
            }
            _ => Err(e),
        }
    }

    /// Drain the CQ. `on_fiber` gets the fiber each op belonged to, after its
    /// `Completion` has been written. Returns whether the wake read completed.
    ///
    /// CQEs are copied out in batches and handled after the `&mut IoUring`
    /// borrow has ended, so `on_fiber` may do anything but reap re-entrantly.
    pub(crate) fn reap(&self, mut on_fiber: impl FnMut(NonNull<FiberHeader>)) -> bool {
        const BATCH: usize = 64;
        let mut woke = false;
        // Uninitialised on purpose: zeroing 1 KiB on every call showed up as a
        // memset-sized loop in each tick, whether one CQE was waiting or none.
        let mut batch = [MaybeUninit::<(u64, i32)>::uninit(); BATCH];
        loop {
            let n = self.with(|ring| {
                let mut cq = ring.completion();
                let mut n = 0;
                while n < BATCH {
                    let Some(cqe) = cq.next() else { break };
                    batch[n].write((cqe.user_data(), cqe.result()));
                    n += 1;
                }
                n
            });
            for slot in &batch[..n] {
                // SAFETY: the first `n` slots were written just above.
                let (raw, res) = unsafe { slot.assume_init() };
                match UserData::decode(raw) {
                    UserData::Wake => {
                        self.wake_inflight.set(false);
                        woke = true;
                    }
                    UserData::Cancel => self.dec_inflight(),
                    UserData::Fiber(completion) => {
                        self.dec_inflight();
                        // SAFETY: invariants 1 and 2. This CQE is the terminal
                        // one of an op still counted in flight, so its fiber is
                        // parked in the `syscall` frame that owns `completion`.
                        let c = unsafe { completion.as_ref() };
                        c.result.set(res);
                        on_fiber(c.fiber);
                    }
                }
            }
            if n < BATCH {
                return woke;
            }
        }
    }

    fn dec_inflight(&self) {
        let n = self.inflight.get();
        check!(n > 0, "CQE arrived with nothing in flight (invariant 3)");
        self.inflight.set(n - 1);
    }

    /// Close the ring. Aborts if anything is still in flight: the kernel would
    /// be left holding pointers into memory that is about to be freed.
    pub(crate) fn close(&self) {
        check!(
            self.inflight.get() == 0 && !self.wake_inflight.get(),
            "{} operation(s) (wake read armed: {}) still in flight at ring teardown (invariant 2)",
            self.inflight.get(),
            self.wake_inflight.get()
        );
        // SAFETY: single thread, and no `with` borrow is active.
        let slot = unsafe { &mut *self.ring.get() };
        *slot = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Prediction: tags round-trip, and a fiber pointer keeps its low bits clear.
    #[test]
    fn user_data_round_trips() {
        let c = Box::new(Completion::new(NonNull::dangling()));
        let p = NonNull::from(&*c);
        let raw = UserData::Fiber(p).encode();
        assert_eq!(raw & TAG_MASK, TAG_FIBER);
        match UserData::decode(raw) {
            UserData::Fiber(q) => assert_eq!(q, p),
            other => panic!("{other:?}"),
        }
        assert!(matches!(
            UserData::decode(UserData::Wake.encode()),
            UserData::Wake
        ));
        assert!(matches!(
            UserData::decode(UserData::Cancel.encode()),
            UserData::Cancel
        ));
    }

    // Prediction: a result read before any CQE aborts (death test), but one
    // written by "the kernel" reads back.
    #[test]
    fn completion_reads_back() {
        let c = Completion::new(NonNull::dangling());
        c.result.set(-4);
        assert_eq!(c.result(), -4);
    }

    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_corrupt_user_data() {
        crate::testutil::death_test(
            "ring::tests::death_corrupt_user_data",
            || {
                UserData::decode(0b11);
            },
            &["corrupt user_data"],
        );
    }

    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_result_before_cqe() {
        crate::testutil::death_test(
            "ring::tests::death_result_before_cqe",
            || {
                let c = Completion::new(NonNull::dangling());
                c.result();
            },
            &["before its completion arrived"],
        );
    }

    // Prediction: with the producer posting one task and waiting for it to be
    // consumed before the next, every post lands as the owner is about to sleep,
    // and none is ever left unseen: the owner follows the sleep protocol (pop;
    // announce; re-check; block in poll on the real eventfd). A lost wakeup
    // would leave the owner polling while the producer waits for it, and show up
    // as a 5 s timeout. (Continuous posting would hide it: the next post rescues.)
    #[test]
    #[cfg_attr(miri, ignore)]
    fn no_post_is_lost_between_the_check_and_the_sleep() {
        use crate::remote::Shared;
        use std::sync::Arc;
        use std::sync::atomic::{AtomicUsize, Ordering};

        const N: usize = 20_000;
        let s = Arc::new(Shared::new(new_eventfd().unwrap(), 4));
        let consumed = Arc::new(AtomicUsize::new(0));
        let producer = {
            let (s, consumed) = (s.clone(), consumed.clone());
            std::thread::spawn(move || {
                for i in 0..N {
                    s.post(Box::new(|_| {})).unwrap();
                    let mut spins = 0u32;
                    while consumed.load(Ordering::Acquire) <= i {
                        // Yield after a short spin: on a loaded or single-CPU
                        // box a pure spin starves the consumer it waits for.
                        spins += 1;
                        if spins < 64 {
                            std::hint::spin_loop();
                        } else {
                            std::thread::yield_now();
                        }
                    }
                }
            })
        };
        let mut got = 0;
        while got < N {
            if s.pop().is_some() {
                got += 1;
                consumed.store(got, Ordering::Release);
                continue;
            }
            if !s.announce_sleep() {
                let mut fds = libc::pollfd {
                    fd: s.eventfd(),
                    events: libc::POLLIN,
                    revents: 0,
                };
                // SAFETY: one valid pollfd that outlives the call.
                let ready = unsafe { libc::poll(&raw mut fds, 1, 5000) };
                assert!(
                    ready > 0,
                    "lost wakeup: slept 5 s with work queued ({got}/{N})"
                );
                let mut counter = [0u8; 8];
                // SAFETY: the buffer is 8 bytes, as an eventfd read requires.
                unsafe { libc::read(s.eventfd(), counter.as_mut_ptr().cast(), 8) };
            }
            s.clear_sleep();
        }
        producer.join().unwrap();
    }

    // Prediction (the kernel fact a deferred timeout cancel relies on): an
    // ASYNC_CANCEL queued BEFORE an operation with the same `user_data` runs
    // before that operation exists, so it finds nothing (ENOENT) and the later
    // read completes normally. A timeout cancel that stays in the SQ after a
    // failed submit therefore cannot hit the fiber's next operation, which
    // reuses the user_data.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn a_queued_cancel_runs_before_a_later_op_with_the_same_user_data() {
        use io_uring::{IoUring, opcode, types::Fd};
        use std::io::Write;
        use std::os::fd::AsRawFd;

        let mut ring = IoUring::new(8).unwrap();
        let (r, mut w) = std::io::pipe().unwrap();
        let mut byte = [0u8; 1];
        let cancel = opcode::AsyncCancel::new(7).build().user_data(100);
        let read = opcode::Read::new(Fd(r.as_raw_fd()), byte.as_mut_ptr(), 1)
            .build()
            .user_data(7);
        // SAFETY: `byte` outlives the read, which completes before the end of
        // the test (a byte is written below and the CQEs are waited for).
        unsafe {
            let mut sq = ring.submission();
            sq.push(&cancel).unwrap();
            sq.push(&read).unwrap();
        }
        ring.submit().unwrap();
        w.write_all(b"x").unwrap();
        ring.submit_and_wait(2).unwrap();
        let mut results: Vec<(u64, i32)> = ring
            .completion()
            .map(|c| (c.user_data(), c.result()))
            .collect();
        results.sort();
        assert_eq!(results, [(7, 1), (100, -libc::ENOENT)]);
    }

    // Prediction: with TWO producers racing to post into a sleeping owner, one
    // post outstanding each, none is lost. Each round both post at the same
    // moment and wait for both to be consumed, so one producer takes the flag
    // down and writes the eventfd while the other's flag load saw it up and its
    // swap saw it down: the case the single-producer test above cannot reach. A
    // lost wakeup would leave the owner polling while a producer waits for it,
    // and show up as a 5 s timeout.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn no_post_is_lost_with_two_racing_producers() {
        use crate::remote::Shared;
        use std::sync::Arc;
        use std::sync::atomic::{AtomicUsize, Ordering};

        const ROUNDS: usize = 10_000;
        let s = Arc::new(Shared::new(new_eventfd().unwrap(), 4));
        let consumed = Arc::new(AtomicUsize::new(0));
        let producers: Vec<_> = (0..2)
            .map(|_| {
                let (s, consumed) = (s.clone(), consumed.clone());
                std::thread::spawn(move || {
                    for round in 0..ROUNDS {
                        // Both producers leave the previous round together.
                        wait_for(&consumed, 2 * round);
                        s.post(Box::new(|_| {})).unwrap();
                        wait_for(&consumed, 2 * round + 2);
                    }
                })
            })
            .collect();
        let mut got = 0;
        while got < 2 * ROUNDS {
            if s.pop().is_some() {
                got += 1;
                consumed.store(got, Ordering::Release);
                continue;
            }
            if !s.announce_sleep() {
                let mut fds = libc::pollfd {
                    fd: s.eventfd(),
                    events: libc::POLLIN,
                    revents: 0,
                };
                // SAFETY: one valid pollfd that outlives the call.
                let ready = unsafe { libc::poll(&raw mut fds, 1, 5000) };
                assert!(
                    ready > 0,
                    "lost wakeup: slept 5 s with work queued ({got}/{})",
                    2 * ROUNDS
                );
                let mut counter = [0u8; 8];
                // SAFETY: the buffer is 8 bytes, as an eventfd read requires.
                unsafe { libc::read(s.eventfd(), counter.as_mut_ptr().cast(), 8) };
            }
            s.clear_sleep();
        }
        for p in producers {
            p.join().unwrap();
        }
    }

    /// Wait until `counter` reaches `target`: a short spin, then yield, so a
    /// loaded or single-CPU machine does not starve the thread being waited for.
    fn wait_for(counter: &std::sync::atomic::AtomicUsize, target: usize) {
        let mut spins = 0u32;
        while counter.load(std::sync::atomic::Ordering::Acquire) < target {
            spins += 1;
            if spins < 64 {
                std::hint::spin_loop();
            } else {
                std::thread::yield_now();
            }
        }
    }
}
