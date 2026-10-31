#![forbid(unsafe_code)]
//! `Io`: owns the thread's scheduler and, through it, the ring. `!Send`:
//! create it on the thread that runs it. Holds owning handles only (Rule 12):
//! nothing the kernel or a fiber points at lives inline, so `let io2 = io;`
//! is harmless.
//!
//! The loop is the C++ pipeline (Rule 7): resume, then submit-or-wait, then
//! reap, with the quiesce check before any blocking wait.

use std::io;
use std::marker::PhantomData;
use std::rc::Rc;
use std::sync::Arc;

use crate::check::{error, fatal};
use crate::cx::{Cx, NotSend};
use crate::detail::fiber::FiberId;
use crate::options::Options;
use crate::remote::Remote;
use crate::sched::Sched;

/// A drain gives up (by aborting) after this many consecutive failed ticks.
/// Returning instead would leave operations in flight; the ring teardown would
/// then free memory the kernel still points at.
const MAX_TICK_ERRORS: u32 = 3;

pub struct Io {
    sched: Rc<Sched>,
    _p: NotSend,
}

impl Io {
    pub fn new(opts: Options) -> io::Result<Io> {
        let opts = opts.validate()?;
        let sched = Sched::new(opts);
        sched.open_ring()?;
        Ok(Io {
            sched,
            _p: PhantomData,
        })
    }

    #[inline]
    pub(crate) fn sched(&self) -> &Rc<Sched> {
        &self.sched
    }

    /// Queue a fiber. It starts when the loop next runs. `ECANCELED` once the
    /// runtime has been stopped.
    pub fn spawn<F>(&self, f: F) -> io::Result<FiberId>
    where
        F: FnOnce(&Cx) + 'static,
    {
        self.sched.spawn(f)
    }

    /// A `Send` handle other threads use to post work and stop the loop.
    pub fn remote(&self) -> Remote {
        Remote::new(Arc::clone(self.sched.shared()))
    }

    /// One turn of the loop. `draining` makes it return instead of sleeping
    /// once quiesced (invariant 8): a drain decides when to stop, so a tick
    /// must not wait for a completion that can never arrive.
    fn tick(&self, draining: bool) -> io::Result<()> {
        // Phase 1: posted closures become fibers, then resume. Resuming is
        // what queues SQEs, so doing it first means the submit below carries
        // them into the kernel this tick.
        self.sched.invalidate_clock();
        self.sched.drain_remote();
        self.sched.resume_ready(self.sched.opts().batch_max_size);

        // Quiesce check, BEFORE the blocking wait: phase 1 may just have
        // finished the last fiber, and waiting now would wait for a CQE that
        // never comes. Omitting it hangs the shutdown tests.
        if draining && self.sched.quiesced() {
            return Ok(());
        }

        // Phase 2: submit, and sleep only when nothing is runnable.
        let submitted = self.sched.submit_or_wait();

        // Phase 3: reap, even if the submit failed: completions already in the
        // CQ still have to wake their fibers. They only mark fibers ready; the
        // next tick's phase 1 resumes them. Then re-queue the eventfd read.
        self.sched.reap();
        self.sched.arm_wake();
        self.sched.process_expired_timers();
        submitted
    }

    /// Tick during a drain. A drain never gives up on a tick error: returning
    /// would leave operations in flight and the ring teardown would free memory
    /// the kernel still points at, so it retries and aborts only after
    /// `MAX_TICK_ERRORS` consecutive failures.
    fn drain_tick(&self, who: &str, errors: &mut u32) {
        match self.tick(true) {
            Ok(()) => *errors = 0,
            Err(e) => {
                error(format_args!("{who}: tick failed: {e}"));
                *errors += 1;
                if *errors >= MAX_TICK_ERRORS {
                    fatal(format_args!(
                        "{who}: {} operation(s) still in flight after {errors} failed ticks; \
                         tearing the ring down now would be a kernel use-after-free",
                        self.sched.inflight()
                    ));
                }
            }
        }
    }

    /// Run fibers until nothing is ready, in flight or posted (invariant 9). A
    /// fiber still live then can never be woken, which is a bug: it aborts,
    /// naming the primitive (invariant 10). `Remote::stop` makes it cancel and
    /// drain instead, then return.
    pub fn run_until_done(&mut self) {
        self.sched.activate();
        let mut errors = 0;
        while !self.sched.quiesced() {
            if self.sched.stop_requested() {
                self.cancel_and_drain();
                return;
            }
            self.drain_tick("run_until_done", &mut errors);
        }
        if self.sched.stop_requested() {
            self.cancel_and_drain();
            return;
        }
        self.sched.abort_if_started_live("quiesced");
    }

    /// Run until `Remote::stop`, sleeping in the kernel whenever nothing is
    /// runnable, then cancel what is in flight and drain. A stopped `Io` stays
    /// stopped: `run` and `run_until_done` return at once and `spawn` fails
    /// with `ECANCELED`. A loop that keeps failing (three ticks in a row) also
    /// ends in the drain.
    pub fn run(&mut self) {
        if self.sched.stopping() {
            return;
        }
        self.sched.activate();
        let mut errors = 0;
        while !self.sched.stop_requested() {
            match self.tick(false) {
                Ok(()) => errors = 0,
                Err(e) => {
                    error(format_args!("run: tick failed: {e}"));
                    errors += 1;
                    if errors >= MAX_TICK_ERRORS {
                        error(format_args!(
                            "run: {errors} failed ticks in a row; draining"
                        ));
                        break;
                    }
                }
            }
        }
        self.cancel_and_drain();
    }

    /// The shutdown sequence (section 6): close the post queue, stop accepting
    /// work, cancel every request in flight, and tick until nothing is ready,
    /// in flight or posted. Fibers parked in I/O wake with `ECANCELED`; fibers
    /// parked on a sync primitive are never woken by the runtime (invariant
    /// 10), so one still live afterwards aborts. Finally drop any closure that
    /// was posted but never became a fiber.
    fn cancel_and_drain(&self) {
        self.sched.close_posts();
        self.sched.begin_stop();
        let mut cancel_sent = self.sched.inflight() == 0;
        let mut errors = 0;
        while !self.sched.quiesced() {
            if !cancel_sent {
                cancel_sent = self.sched.try_cancel_all();
            }
            self.drain_tick("cancel_and_drain", &mut errors);
        }
        self.sched
            .abort_if_started_live("cancel_and_drain: quiesced");
        self.sched.discard_posts();
    }
}

impl Drop for Io {
    /// Never runs fibers (section 6, "Shutdown"). Aborts if an operation is in
    /// flight or a started fiber is unfinished; drops never-started closures
    /// without running them; then closes the ring and frees the stacks.
    fn drop(&mut self) {
        self.sched.teardown();
    }
}

#[cfg(test)]
mod tests {
    use std::cell::{Cell, RefCell};
    use std::io::Write;
    use std::rc::Rc;
    use std::time::Duration;

    use super::*;
    use crate::cx::is_cancelled;
    use crate::detail::fiber::WaitKind;
    use crate::testutil::{death_test, watchdog};

    fn io() -> Io {
        watchdog();
        Io::new(Options::default()).unwrap()
    }

    // Prediction: with four ops parked in the kernel (three pipe reads, one 1-hour
    // sleep), cancel_and_drain ends with nothing in flight, every fiber saw
    // ECANCELED, and it does not hang.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn cancel_and_drain_cancels_every_op() {
        let mut pipes = Vec::new();
        let io = io();
        let outcomes = Rc::new(RefCell::new(Vec::new()));
        for _ in 0..3 {
            let (r, w) = std::io::pipe().unwrap();
            pipes.push(w); // keep the write end open so the read stays pending
            let o = outcomes.clone();
            io.spawn(move |cx| {
                let mut buf = [0u8; 1];
                let e = cx.read(&r, &mut buf, None).unwrap_err();
                o.borrow_mut().push(("read", is_cancelled(&e)));
            })
            .unwrap();
        }
        let o = outcomes.clone();
        io.spawn(move |cx| {
            let e = cx.sleep(Duration::from_secs(3600)).unwrap_err();
            o.borrow_mut().push(("sleep", is_cancelled(&e)));
        })
        .unwrap();

        // Start them, and let the kernel see the ops. (Not `tick`: with nothing
        // runnable it would, correctly, block waiting for a CQE.)
        io.sched().resume_ready(16);
        io.sched().submit_now();
        assert_eq!(io.sched().inflight(), 4);

        io.cancel_and_drain();

        assert_eq!(io.sched().inflight(), 0);
        let o = outcomes.borrow();
        assert_eq!(o.len(), 4, "{o:?}");
        assert!(o.iter().all(|&(_, cancelled)| cancelled), "{o:?}");
        assert!(o.iter().any(|&(what, _)| what == "sleep"));
    }

    // Prediction: ops still sitting unsubmitted in the SQ are cancelled too (the
    // cancel is queued behind them and submitted together).
    #[test]
    #[cfg_attr(miri, ignore)]
    fn cancel_and_drain_catches_unsubmitted_ops() {
        let (r, _w) = std::io::pipe().unwrap();
        let io = io();
        let cancelled = Rc::new(Cell::new(false));
        let c = cancelled.clone();
        io.spawn(move |cx| {
            let mut buf = [0u8; 1];
            c.set(is_cancelled(&cx.read(&r, &mut buf, None).unwrap_err()));
        })
        .unwrap();
        io.sched().resume_ready(4); // the SQE is queued; no tick has submitted it
        assert_eq!(io.sched().inflight(), 1);
        io.cancel_and_drain();
        assert!(cancelled.get());
        assert_eq!(io.sched().inflight(), 0);
    }

    // Prediction: draining an idle runtime returns at once, and afterwards the Io
    // is stopped: spawn, yield, I/O and run_until_done all behave as stopped.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn a_stopped_io_refuses_work() {
        let mut io = io();
        io.cancel_and_drain();
        let e = io.spawn(|_| {}).unwrap_err();
        assert!(is_cancelled(&e));
        io.run_until_done(); // returns at once
    }

    // Prediction (invariant 11): a fiber that checks the result of yield_now exits
    // when the drain starts; ready fibers are not stranded.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn stop_makes_yield_fail_and_fibers_exit() {
        let io = io();
        let exited = Rc::new(Cell::new(false));
        let e = exited.clone();
        io.spawn(move |cx| {
            while cx.yield_now().is_ok() {}
            e.set(true);
        })
        .unwrap();
        io.sched().resume_ready(1); // it yields once and is ready again
        io.cancel_and_drain();
        assert!(exited.get());
    }

    // Prediction: after the stop, I/O is refused before it reaches the ring, and a
    // refused close still closes the descriptor (the OwnedFd drops): the pipe's
    // read end then sees EOF instead of blocking. (If the fd leaked, that read
    // would block and the watchdog would abort the run.)
    #[test]
    #[cfg_attr(miri, ignore)]
    fn refused_close_still_closes_the_fd() {
        use std::io::Read;
        let (r, w) = std::io::pipe().unwrap();
        let io = io();
        let result = Rc::new(Cell::new(None));
        let res = result.clone();
        io.spawn(move |cx| {
            // Park first so the stop lands while this fiber is mid-flight.
            cx.sched().park(WaitKind::Io);
            let fd = std::os::fd::OwnedFd::from(w);
            res.set(Some(cx.close(fd).map_err(|e| is_cancelled(&e))));
        })
        .unwrap();
        io.sched().resume_ready(1);
        io.sched().begin_stop();
        // Wake the parked fiber the way a CQE would, then let it run.
        let h = io.sched().headers()[0];
        io.sched().push_ready(h);
        io.sched().resume_ready(1);
        assert_eq!(result.take(), Some(Err(true)));
        let mut buf = [0u8; 1];
        assert_eq!((&r).read(&mut buf).unwrap(), 0, "write end still open");
    }

    // Prediction: dropping an Io with an op still in flight aborts and says why
    // (the kernel would keep pointers into freed stacks).
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_drop_with_op_in_flight() {
        death_test(
            "io::tests::death_drop_with_op_in_flight",
            || {
                let io = io();
                io.spawn(|cx| {
                    let _ = cx.sleep(Duration::from_secs(3600));
                })
                .unwrap();
                io.sched().resume_ready(1);
                io.sched().submit_now();
                drop(io);
            },
            &["1 operation(s) still in flight", "invariant 2"],
        );
    }

    // Prediction (invariant 10): a fiber parked on a Mutex is never woken by the
    // drain, so it aborts, naming the Mutex, rather than unwinding the fiber.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_drain_with_sync_parked_fiber() {
        death_test(
            "io::tests::death_drain_with_sync_parked_fiber",
            || {
                let io = io();
                io.spawn(|cx| cx.sched().park(WaitKind::Mutex)).unwrap();
                io.cancel_and_drain();
            },
            &["cancel_and_drain: quiesced", "1 on Mutex (fiber 1)"],
        );
    }

    // Prediction: writing to a pipe whose reader is gone fails with EPIPE, not a signal kill
    // of the process we are testing in (io_uring reports it as a result code; the process
    // would only die on SIGPIPE if it were raised, which io_uring does not do for this op).
    #[test]
    #[cfg_attr(miri, ignore)]
    fn broken_pipe_is_an_error_not_a_signal() {
        let (r, mut w) = std::io::pipe().unwrap();
        drop(r);
        let mut io = io();
        let code = Rc::new(Cell::new(0));
        let c = code.clone();
        io.spawn(move |cx| {
            let e = cx.write(&w, b"x", None).unwrap_err();
            c.set(e.raw_os_error().unwrap_or(-1));
            let _ = w.flush();
        })
        .unwrap();
        io.run_until_done();
        assert_eq!(code.get(), libc::EPIPE);
    }
}
