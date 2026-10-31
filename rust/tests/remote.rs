#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Layer 5: `Remote`, `run`, `stop` and the shutdown sequence. Each test
//! states its prediction first.

mod common;

use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use std::thread;
use std::time::{Duration, Instant};

use common::watchdog;
use tinyio::{Io, Mutex, Options, PostError, is_cancelled, scope};

const MS: Duration = Duration::from_millis(1);

fn io() -> Io {
    watchdog(60);
    Io::new(Options::default()).unwrap()
}

/// A pipe whose write end the test keeps open, so a read on it stays pending.
fn pending_pipe() -> (std::io::PipeReader, std::io::PipeWriter) {
    std::io::pipe().unwrap()
}

// Prediction: a closure posted from another thread runs on the owner thread, as
// a fiber (it can sleep), and the owner, blocked in the kernel until then, wakes for it.
#[test]
fn post_runs_on_the_owner_thread() {
    let mut io = io();
    let remote = io.remote();
    let owner = thread::current().id();
    let seen = Arc::new(StdMutex::new(None));
    let (s, stopper) = (seen.clone(), remote.clone());
    let t = thread::spawn(move || {
        thread::sleep(20 * MS);
        remote
            .post(move |cx| {
                cx.sleep(MS).unwrap();
                *s.lock().unwrap() = Some(thread::current().id());
                stopper.stop();
            })
            .unwrap();
    });
    io.run();
    t.join().unwrap();
    assert_eq!(*seen.lock().unwrap(), Some(owner));
}

// Prediction: 8 threads posting 500 closures each while the owner sleeps in
// `run`: every one runs, and the owner stops itself on the last.
#[test]
fn eight_threads_post_while_the_owner_sleeps() {
    const THREADS: usize = 8;
    const EACH: usize = 500;
    let mut io = io();
    let remote = io.remote();
    let ran = Arc::new(AtomicUsize::new(0));
    let producers: Vec<_> = (0..THREADS)
        .map(|_| {
            let (remote, ran) = (remote.clone(), ran.clone());
            thread::spawn(move || {
                thread::sleep(30 * MS); // the owner is asleep in the kernel by now
                for _ in 0..EACH {
                    loop {
                        let (r, ran) = (remote.clone(), ran.clone());
                        let post = remote.post(move |_| {
                            if ran.fetch_add(1, Ordering::SeqCst) + 1 == THREADS * EACH {
                                r.stop();
                            }
                        });
                        match post {
                            Ok(()) => break,
                            Err(PostError::Full) => thread::yield_now(),
                            Err(PostError::Closed) => panic!("closed early"),
                        }
                    }
                }
            })
        })
        .collect();
    io.run();
    for p in producers {
        p.join().unwrap();
    }
    assert_eq!(ran.load(Ordering::SeqCst), THREADS * EACH);
}

// Prediction: `stop` cancels fibers waiting in I/O; each sees ECANCELED, `run`
// returns promptly, and nothing is left in flight (the Io then drops cleanly).
#[test]
fn stop_cancels_fibers_waiting_in_io() {
    let mut io = io();
    let cancelled = Rc::new(Cell::new(0));
    let mut writers = Vec::new();
    for _ in 0..3 {
        let (r, w) = pending_pipe();
        writers.push(w);
        let c = cancelled.clone();
        io.spawn(move |cx| {
            let mut b = [0u8; 1];
            if is_cancelled(&cx.read(&r, &mut b, None).unwrap_err()) {
                c.set(c.get() + 1);
            }
        })
        .unwrap();
    }
    let remote = io.remote();
    let t = thread::spawn(move || {
        thread::sleep(30 * MS);
        remote.stop();
    });
    let started = Instant::now();
    io.run();
    t.join().unwrap();
    assert_eq!(cancelled.get(), 3);
    assert!(started.elapsed() < Duration::from_secs(2));
}

// Prediction: `stop` reaches a loop that never sleeps (a fiber yields forever),
// and `yield_now` then fails so the fiber ends: ready fibers are not stranded.
#[test]
fn stop_ends_fibers_that_keep_yielding() {
    let mut io = io();
    let ended = Rc::new(Cell::new(false));
    let e = ended.clone();
    io.spawn(move |cx| {
        while cx.yield_now().is_ok() {}
        e.set(true);
    })
    .unwrap();
    let remote = io.remote();
    let t = thread::spawn(move || {
        thread::sleep(20 * MS);
        remote.stop();
    });
    io.run();
    t.join().unwrap();
    assert!(ended.get());
}

// Prediction: `stop` before `run` is sticky: run returns at once, and a
// stopped Io refuses new fibers (ECANCELED) and returns from run_until_done.
#[test]
fn stop_before_run_is_sticky() {
    let mut io = io();
    io.remote().stop();
    io.run();
    assert!(is_cancelled(&io.spawn(|_| {}).unwrap_err()));
    io.run(); // again: still returns at once
    io.run_until_done();
}

// Prediction: `stop` during run_until_done cancels the sleeper (which would
// otherwise take a minute) and returns.
#[test]
fn stop_during_run_until_done() {
    let mut io = io();
    let cancelled = Rc::new(Cell::new(false));
    let c = cancelled.clone();
    io.spawn(move |cx| {
        c.set(is_cancelled(
            &cx.sleep(Duration::from_secs(60)).unwrap_err(),
        ))
    })
    .unwrap();
    let remote = io.remote();
    let t = thread::spawn(move || {
        thread::sleep(30 * MS);
        remote.stop();
    });
    let started = Instant::now();
    io.run_until_done();
    t.join().unwrap();
    assert!(cancelled.get());
    assert!(started.elapsed() < Duration::from_secs(2));
}

// Prediction: a post that arrives while run_until_done waits on a sleeper wakes
// the loop and runs, and run_until_done does not return before it has.
#[test]
fn post_wakes_run_until_done() {
    let mut io = io();
    io.spawn(|cx| cx.sleep(80 * MS).unwrap()).unwrap();
    let remote = io.remote();
    let ran_at = Arc::new(StdMutex::new(None));
    let r = ran_at.clone();
    let started = Instant::now();
    let t = thread::spawn(move || {
        thread::sleep(20 * MS);
        remote
            .post(move |_| *r.lock().unwrap() = Some(started.elapsed()))
            .unwrap();
    });
    io.run_until_done();
    t.join().unwrap();
    let at = ran_at.lock().unwrap().expect("posted closure never ran");
    assert!(
        at < 70 * MS,
        "ran at {at:?}: the post waited for the sleeper"
    );
}

struct CountDrop(Arc<AtomicUsize>);
impl Drop for CountDrop {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

// Prediction: with room for 2, the third post is Full and its closure is
// dropped unrun; the first two run.
#[test]
fn full_queue_refuses_and_drops() {
    let opts = Options {
        post_queue_capacity: 2,
        ..Options::default()
    };
    let mut io = Io::new(opts).unwrap();
    let remote = io.remote();
    let (ran, dropped) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
    let mut results = Vec::new();
    for _ in 0..3 {
        let (ran, guard) = (ran.clone(), CountDrop(dropped.clone()));
        results.push(remote.post(move |_| {
            let _keep = &guard;
            ran.fetch_add(1, Ordering::SeqCst);
        }));
    }
    assert_eq!(results, [Ok(()), Ok(()), Err(PostError::Full)]);
    assert_eq!(dropped.load(Ordering::SeqCst), 1);
    io.run_until_done();
    assert_eq!(ran.load(Ordering::SeqCst), 2);
    assert_eq!(dropped.load(Ordering::SeqCst), 3);
}

// Prediction: once the loop has shut down, and once the Io is gone, every post is Closed.
#[test]
fn post_is_closed_after_shutdown_and_after_drop() {
    let mut io = io();
    let remote = io.remote();
    remote.stop();
    io.run();
    assert_eq!(remote.post(|_| {}), Err(PostError::Closed));
    drop(io);
    assert_eq!(remote.post(|_| {}), Err(PostError::Closed));

    let io2 = Io::new(Options::default()).unwrap();
    let r2 = io2.remote();
    drop(io2); // never ran: dropping must close the queue too
    assert_eq!(r2.post(|_| {}), Err(PostError::Closed));
}

// Prediction (invariant 13): with 4 threads posting flat out while the loop is
// stopped, no post succeeds after the close (every thread ends on Closed), and
// every closure ever created is either run or dropped: none leaks.
#[test]
fn shutdown_under_posting_load_loses_nothing() {
    let mut io = io();
    let remote = io.remote();
    let (made, ran, dropped) = (
        Arc::new(AtomicUsize::new(0)),
        Arc::new(AtomicUsize::new(0)),
        Arc::new(AtomicUsize::new(0)),
    );
    let producers: Vec<_> = (0..4)
        .map(|_| {
            let (remote, made, ran, dropped) =
                (remote.clone(), made.clone(), ran.clone(), dropped.clone());
            thread::spawn(move || {
                loop {
                    made.fetch_add(1, Ordering::SeqCst);
                    let (ran, guard) = (ran.clone(), CountDrop(dropped.clone()));
                    let post = remote.post(move |_| {
                        let _keep = &guard;
                        ran.fetch_add(1, Ordering::SeqCst);
                    });
                    match post {
                        Err(PostError::Closed) => return,
                        Err(PostError::Full) => thread::yield_now(),
                        Ok(()) => {}
                    }
                }
            })
        })
        .collect();
    let stopper = remote.clone();
    let t = thread::spawn(move || {
        thread::sleep(30 * MS);
        stopper.stop();
    });
    io.run();
    assert_eq!(remote.post(|_| {}), Err(PostError::Closed));
    t.join().unwrap();
    for p in producers {
        p.join().unwrap();
    }
    drop(io);
    drop(remote); // the last handle frees the queue, and any closure left in it
    assert!(
        ran.load(Ordering::SeqCst) > 0,
        "nothing ran before the stop"
    );
    assert_eq!(dropped.load(Ordering::SeqCst), made.load(Ordering::SeqCst));
}

// Prediction (Rule 12): run, move the Io, post from another thread, run again:
// nothing the kernel or the thread points at lived inline in the Io.
#[test]
fn io_moves_then_receives_a_post() {
    let mut io = io();
    io.run_until_done();
    let mut moved = io;
    let remote = moved.remote();
    let ran = Arc::new(AtomicUsize::new(0));
    let r = ran.clone();
    thread::spawn(move || {
        remote.post(move |_| {
            r.fetch_add(1, Ordering::SeqCst);
        })
    })
    .join()
    .unwrap()
    .unwrap();
    moved.run_until_done();
    assert_eq!(ran.load(Ordering::SeqCst), 1);

    let stopper = moved.remote();
    let ran2 = ran.clone();
    thread::spawn(move || {
        stopper
            .post(move |_| {
                ran2.fetch_add(1, Ordering::SeqCst);
            })
            .unwrap();
        stopper.stop();
    })
    .join()
    .unwrap();
    moved.run(); // the wake read survived the move and the first run
    assert!(ran.load(Ordering::SeqCst) >= 1);
}

// Prediction: a mutex held across a cancelled read is handed on during the
// drain: A, then B, then C each get it exactly once, in order, with no abort.
#[test]
fn mutex_handoff_survives_shutdown() {
    let mut io = io();
    let m = Rc::new(Mutex::new(&io, Vec::<&'static str>::new()));
    let (r, _w) = pending_pipe();
    let m1 = m.clone();
    io.spawn(move |cx| {
        let mut g = m1.lock(cx);
        g.push("A");
        let mut b = [0u8; 1];
        assert!(is_cancelled(&cx.read(&r, &mut b, None).unwrap_err()));
        g.push("A-cancelled");
    })
    .unwrap();
    for name in ["B", "C"] {
        let m = m.clone();
        io.spawn(move |cx| m.lock(cx).push(name)).unwrap();
    }
    let remote = io.remote();
    let t = thread::spawn(move || {
        thread::sleep(30 * MS);
        remote.stop();
    });
    io.run();
    t.join().unwrap();
    assert_eq!(*m.try_lock().unwrap(), ["A", "A-cancelled", "B", "C"]);
}

// Prediction: a scope's parent wakes only after its cancelled children have
// finished: both children log before the parent resumes.
#[test]
fn scope_parent_wakes_after_cancelled_children() {
    let mut io = io();
    let log = Rc::new(RefCell::new(Vec::new()));
    let (r1, _w1) = pending_pipe();
    let (r2, _w2) = pending_pipe();
    let l = log.clone();
    io.spawn(move |cx| {
        scope(cx, |s| {
            for (name, pipe) in [("child1", &r1), ("child2", &r2)] {
                let l = &l;
                s.spawn(move |cx| {
                    let mut b = [0u8; 1];
                    assert!(is_cancelled(&cx.read(pipe, &mut b, None).unwrap_err()));
                    l.borrow_mut().push(name);
                })
                .unwrap();
            }
        });
        l.borrow_mut().push("parent");
    })
    .unwrap();
    let remote = io.remote();
    let t = thread::spawn(move || {
        thread::sleep(30 * MS);
        remote.stop();
    });
    io.run();
    t.join().unwrap();
    let log = log.borrow();
    assert_eq!(log.len(), 3, "{log:?}");
    assert_eq!(log[2], "parent");
}

// Prediction: end to end, a post that races the owner's descent into the kernel
// is never lost: across 400 runs with the producer delayed by 0..2000 spins,
// `run` always returns (a lost wakeup would hang and the watchdog would abort).
// This catches a flag that is never raised; the fine-grained ordering of the
// sleep protocol is tested in `ring.rs::no_post_is_lost_between_the_check_and_the_sleep`.
#[test]
fn post_racing_the_owner_going_to_sleep_never_hangs() {
    for i in 0..400 {
        let mut io = io();
        let remote = io.remote();
        let spins = (i % 40) * 50;
        let t = thread::spawn(move || {
            for _ in 0..spins {
                std::hint::spin_loop();
            }
            let stopper = remote.clone();
            remote.post(move |_| stopper.stop()).unwrap();
        });
        io.run();
        t.join().unwrap();
    }
}
