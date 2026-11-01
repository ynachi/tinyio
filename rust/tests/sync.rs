#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Layer 4 (behaviour): the sync primitives and `scope`. Each test states its
//! prediction first.

mod common;

use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::Duration;

use common::watchdog;
use tinyio::{Baton, Io, Latch, Mutex, Options, Semaphore, scope};

fn io() -> Io {
    watchdog(60);
    Io::new(Options::default()).unwrap()
}

fn log() -> Rc<RefCell<Vec<String>>> {
    Rc::new(RefCell::new(Vec::new()))
}

// Prediction: two fibers that yield inside the critical section never interleave there.
#[test]
fn mutex_excludes_across_a_suspend() {
    let mut io = io();
    let m = Rc::new(Mutex::new(&io, 0u32));
    let torn = Rc::new(Cell::new(false));
    for _ in 0..2 {
        let (m, torn) = (m.clone(), torn.clone());
        io.spawn(move |cx| {
            for _ in 0..50 {
                let mut g = m.lock(cx).unwrap();
                let before = *g;
                cx.yield_now().unwrap();
                if *g != before {
                    torn.set(true);
                }
                *g += 1;
            }
        })
        .unwrap();
    }
    io.run_until_done();
    assert!(!torn.get());
    assert_eq!(*m.try_lock().unwrap(), 100);
}

// Prediction: waiters get the lock in arrival order.
#[test]
fn mutex_is_fifo() {
    let mut io = io();
    let m = Rc::new(Mutex::new(&io, ()));
    let order = log();
    let (m0, o0) = (m.clone(), order.clone());
    io.spawn(move |cx| {
        let g = m0.lock(cx).unwrap();
        cx.yield_now().unwrap(); // let 1, 2, 3 queue up behind us
        cx.yield_now().unwrap();
        o0.borrow_mut().push("holder".into());
        drop(g);
    })
    .unwrap();
    for i in 1..=3 {
        let (m, o) = (m.clone(), order.clone());
        io.spawn(move |cx| {
            let _g = m.lock(cx).unwrap();
            o.borrow_mut().push(format!("w{i}"));
        })
        .unwrap();
    }
    io.run_until_done();
    assert_eq!(*order.borrow(), ["holder", "w1", "w2", "w3"]);
}

// Prediction: unlock with a waiter hands the lock over, so try_lock fails
// between the unlock and the waiter running.
#[test]
fn mutex_unlock_hands_over_to_the_waiter() {
    let mut io = io();
    let m = Rc::new(Mutex::new(&io, ()));
    let barged = Rc::new(Cell::new(true));
    let (m1, b) = (m.clone(), barged.clone());
    io.spawn(move |cx| {
        let g = m1.lock(cx).unwrap();
        cx.yield_now().unwrap(); // the second fiber now waits
        drop(g);
        b.set(m1.try_lock().is_some()); // must be None: ownership moved
    })
    .unwrap();
    let m2 = m.clone();
    io.spawn(move |cx| drop(m2.lock(cx).unwrap())).unwrap();
    io.run_until_done();
    assert!(!barged.get());
}

// Prediction: try_lock is None while held and Some after the guard drops.
#[test]
fn try_lock_follows_the_guard() {
    let io = io();
    let m = Mutex::new(&io, 7u8);
    let g = m.try_lock().unwrap();
    assert!(m.try_lock().is_none());
    drop(g);
    assert_eq!(*m.try_lock().unwrap(), 7);
}

// Prediction: with 2 permits and 5 fibers, at most 2 are inside at once, and 2 do get in together.
#[test]
fn semaphore_caps_concurrency() {
    let mut io = io();
    let sem = Rc::new(Semaphore::new(&io, 2));
    let (inside, peak) = (Rc::new(Cell::new(0)), Rc::new(Cell::new(0)));
    for _ in 0..5 {
        let (sem, inside, peak) = (sem.clone(), inside.clone(), peak.clone());
        io.spawn(move |cx| {
            let _permit = sem.acquire(cx).unwrap();
            inside.set(inside.get() + 1);
            peak.set(peak.get().max(inside.get()));
            cx.yield_now().unwrap();
            inside.set(inside.get() - 1);
        })
        .unwrap();
    }
    io.run_until_done();
    assert_eq!(peak.get(), 2);
}

// Prediction: release with a waiter passes the permit on, so try_acquire sees none.
#[test]
fn semaphore_release_hands_over() {
    let mut io = io();
    let sem = Rc::new(Semaphore::new(&io, 1));
    let stolen = Rc::new(Cell::new(true));
    let (s1, st) = (sem.clone(), stolen.clone());
    io.spawn(move |cx| {
        s1.acquire(cx).unwrap().forget();
        cx.yield_now().unwrap();
        s1.release();
        st.set(s1.try_acquire().is_some());
    })
    .unwrap();
    let s2 = sem.clone();
    io.spawn(move |cx| {
        drop(s2.acquire(cx).unwrap());
    })
    .unwrap();
    io.run_until_done();
    assert!(!stolen.get());
}

// Prediction: a latch of 3 releases all of its waiters only after the third count_down.
#[test]
fn latch_releases_waiters_at_zero() {
    let mut io = io();
    let latch = Rc::new(Latch::new(&io, 3));
    let order = log();
    for i in 0..2 {
        let (l, o) = (latch.clone(), order.clone());
        io.spawn(move |cx| {
            l.wait(cx).unwrap();
            o.borrow_mut().push(format!("waiter{i}"));
        })
        .unwrap();
    }
    for i in 0..3 {
        let (l, o) = (latch.clone(), order.clone());
        io.spawn(move |cx| {
            cx.yield_now().unwrap();
            o.borrow_mut().push(format!("count{i}"));
            l.count_down();
        })
        .unwrap();
    }
    io.run_until_done();
    let o = order.borrow();
    assert_eq!(o.len(), 5);
    assert!(
        o[3].starts_with("waiter") && o[4].starts_with("waiter"),
        "{o:?}"
    );
}

// Prediction: waiting on a latch that already reached zero returns at once.
#[test]
fn latch_at_zero_does_not_block() {
    let mut io = io();
    let latch = Latch::new(&io, 0);
    let done = Rc::new(Cell::new(false));
    let d = done.clone();
    let latch = Rc::new(latch);
    io.spawn(move |cx| {
        latch.wait(cx).unwrap();
        d.set(true);
    })
    .unwrap();
    io.run_until_done();
    assert!(done.get());
}

// Prediction: a baton posted before the wait returns at once; posting twice is harmless.
#[test]
fn baton_post_then_wait() {
    let mut io = io();
    let b = Rc::new(Baton::new(&io));
    b.post();
    b.post();
    let done = Rc::new(Cell::new(false));
    let (b2, d) = (b.clone(), done.clone());
    io.spawn(move |cx| {
        b2.wait(cx).unwrap();
        d.set(true);
    })
    .unwrap();
    io.run_until_done();
    assert!(done.get() && b.is_posted());
}

// Prediction: a waiter parks until another fiber posts, and runs after the poster yields.
#[test]
fn baton_wait_then_post() {
    let mut io = io();
    let b = Rc::new(Baton::new(&io));
    let order = log();
    let (b1, o1) = (b.clone(), order.clone());
    io.spawn(move |cx| {
        b1.wait(cx).unwrap();
        o1.borrow_mut().push("woken".into());
    })
    .unwrap();
    let (b2, o2) = (b.clone(), order.clone());
    io.spawn(move |_| {
        o2.borrow_mut().push("post".into());
        b2.post();
        o2.borrow_mut().push("after-post".into());
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(*order.borrow(), ["post", "after-post", "woken"]);
}

// Prediction: scoped children borrow and mutate locals of the parent, and
// scope returns only after both have finished.
#[test]
fn scope_borrows_locals_and_joins() {
    let mut io = io();
    let result = Rc::new(Cell::new(0));
    let r = result.clone();
    io.spawn(move |cx| {
        let mut total = 0;
        let words = ["a", "bb", "ccc"];
        scope(cx, |s| {
            s.spawn(|cx| {
                cx.sleep(Duration::from_millis(5)).unwrap();
                total += words.iter().map(|w| w.len()).sum::<usize>();
            })
            .unwrap();
            s.spawn(|cx| cx.yield_now().unwrap()).unwrap();
        });
        r.set(total); // only valid because both children finished
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(result.get(), 6);
}

// Prediction: a child may spawn siblings through the same scope; all are joined.
#[test]
fn scope_children_spawn_children() {
    let mut io = io();
    let count = Rc::new(Cell::new(0));
    let c = count.clone();
    io.spawn(move |cx| {
        scope(cx, |s| {
            s.spawn(|_| {
                for _ in 0..3 {
                    s.spawn(|_| c.set(c.get() + 1)).unwrap();
                }
            })
            .unwrap();
        });
        assert_eq!(c.get(), 3);
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(count.get(), 3);
}

// Prediction (Rule 13): a scoped child that locks a *contended* mutex through its
// parent's `Cx` parks the child, not the parent: the parent is waiting in the
// scope's join, and queueing it twice would abort. The child waits for the holder
// and then gets the lock.
#[test]
fn scoped_child_may_use_the_parents_cx() {
    let mut io = io();
    let m = Rc::new(Mutex::new(&io, 0u8));
    let m1 = m.clone();
    io.spawn(move |cx| {
        let mut g = m1.lock(cx).unwrap();
        cx.yield_now().unwrap(); // the scope below starts while we hold the lock
        cx.yield_now().unwrap();
        *g = 10;
    })
    .unwrap();
    let m2 = m.clone();
    io.spawn(move |cx| {
        scope(cx, |s| {
            // `cx` is the parent's; the child contends for the held lock with it.
            s.spawn(|_| *m2.lock(cx).unwrap() += 1).unwrap();
        });
        assert_eq!(*m2.try_lock().unwrap(), 11);
    })
    .unwrap();
    io.run_until_done();
}

// Prediction (Rule 13): a guard taken by one fiber may be dropped by another.
#[test]
fn guard_can_be_dropped_by_another_fiber() {
    let mut io = io();
    let slot: Rc<RefCell<Option<tinyio::MutexGuard<'static, ()>>>> = Rc::new(RefCell::new(None));
    // Fibers are 'static, so the guard they pass around needs a 'static mutex.
    let leaked: &'static Mutex<()> = Box::leak(Box::new(Mutex::new(&io, ())));
    let s1 = slot.clone();
    io.spawn(move |cx| {
        *s1.borrow_mut() = Some(leaked.lock(cx).unwrap());
    })
    .unwrap();
    let s2 = slot.clone();
    io.spawn(move |cx| {
        cx.yield_now().unwrap();
        drop(s2.borrow_mut().take()); // another fiber releases it
        assert!(leaked.try_lock().is_some());
    })
    .unwrap();
    io.run_until_done();
}

// Prediction: nested scopes join inner before outer.
#[test]
fn nested_scopes_join_inside_out() {
    let mut io = io();
    let order = log();
    let o = order.clone();
    io.spawn(move |cx| {
        scope(cx, |outer| {
            outer
                .spawn(|cx| {
                    scope(cx, |inner| {
                        inner
                            .spawn(|_| o.borrow_mut().push("inner-child".into()))
                            .unwrap();
                    });
                    o.borrow_mut().push("outer-child".into());
                })
                .unwrap();
        });
        o.borrow_mut().push("after".into());
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(*order.borrow(), ["inner-child", "outer-child", "after"]);
}

// Prediction: `stop` wakes fibers parked on a Mutex, Semaphore, Latch and Baton
// with ECANCELED, and the loop returns; nothing aborts. A wait begun after the
// stop fails at once instead of parking, and the primitives stay consistent.
#[test]
fn stop_wakes_every_kind_of_sync_waiter() {
    let mut io = io();
    let remote = io.remote();
    let m = Rc::new(Mutex::new(&io, ()));
    let sem = Rc::new(Semaphore::new(&io, 0));
    let latch = Rc::new(Latch::new(&io, 1));
    let baton = Rc::new(Baton::new(&io));
    let seen = log();

    let (m1, s) = (m.clone(), seen.clone());
    io.spawn(move |cx| {
        let _held = m1.lock(cx).unwrap();
        let r = cx
            .sleep(Duration::from_secs(3600))
            .map_err(|e| tinyio::is_cancelled(&e));
        s.borrow_mut().push(format!("holder {r:?}"));
    })
    .unwrap();
    let (m2, s) = (m.clone(), seen.clone());
    io.spawn(move |cx| {
        let first = m2.lock(cx).map(drop).map_err(|e| tinyio::is_cancelled(&e));
        let again = m2.lock(cx).map(drop).map_err(|e| tinyio::is_cancelled(&e));
        s.borrow_mut().push(format!("mutex {first:?} {again:?}"));
    })
    .unwrap();
    let (sem2, s) = (sem.clone(), seen.clone());
    io.spawn(move |cx| {
        let r = sem2
            .acquire(cx)
            .map(drop)
            .map_err(|e| tinyio::is_cancelled(&e));
        s.borrow_mut().push(format!("semaphore {r:?}"));
    })
    .unwrap();
    let (l, s) = (latch.clone(), seen.clone());
    io.spawn(move |cx| {
        let r = l.wait(cx).map_err(|e| tinyio::is_cancelled(&e));
        s.borrow_mut().push(format!("latch {r:?}"));
    })
    .unwrap();
    let (b, s) = (baton.clone(), seen.clone());
    io.spawn(move |cx| {
        let r = b.wait(cx).map_err(|e| tinyio::is_cancelled(&e));
        s.borrow_mut().push(format!("baton {r:?}"));
    })
    .unwrap();

    let stopper = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(150));
        remote.stop();
    });
    io.run();
    stopper.join().unwrap();

    let mut got = seen.borrow().clone();
    got.sort();
    assert_eq!(
        got,
        [
            "baton Err(true)",
            "holder Err(true)",
            "latch Err(true)",
            "mutex Err(true) Err(true)",
            "semaphore Err(true)",
        ]
    );
    // Nothing was handed a permit or a lock on the way out.
    assert!(m.try_lock().is_some());
    assert!(sem.try_acquire().is_none());
    assert!(!baton.is_posted());
}

// Prediction: a Permit gives its permit back when dropped, including on an
// early return; `forget` keeps it taken until `release`.
#[test]
fn permit_returns_on_drop_and_forget_keeps_it() {
    let mut io = io();
    let sem = Rc::new(Semaphore::new(&io, 1));
    let s = sem.clone();
    io.spawn(move |cx| {
        let early = |cx: &tinyio::Cx| -> Result<(), ()> {
            let _p = s.acquire(cx).map_err(|_| ())?;
            Err(()) // leaves without an explicit release
        };
        assert!(early(cx).is_err());
        let p = s.try_acquire().expect("the permit came back");
        p.forget();
        assert!(s.try_acquire().is_none(), "forget keeps it taken");
        s.release();
        assert!(s.try_acquire().is_some());
    })
    .unwrap();
    io.run_until_done();
}

// Prediction: waiters on a Semaphore are served strictly in the order they arrived.
#[test]
fn semaphore_is_fifo() {
    let mut io = io();
    let sem = Rc::new(Semaphore::new(&io, 1));
    let order = log();
    let (s0, o0) = (sem.clone(), order.clone());
    io.spawn(move |cx| {
        let p = s0.acquire(cx).unwrap();
        for _ in 0..4 {
            cx.yield_now().unwrap(); // let 1, 2, 3 queue up behind us
        }
        o0.borrow_mut().push("holder".into());
        drop(p);
    })
    .unwrap();
    for i in 1..=3 {
        let (s, o) = (sem.clone(), order.clone());
        io.spawn(move |cx| {
            let _p = s.acquire(cx).unwrap();
            o.borrow_mut().push(format!("w{i}"));
        })
        .unwrap();
    }
    io.run_until_done();
    assert_eq!(*order.borrow(), ["holder", "w1", "w2", "w3"]);
}
