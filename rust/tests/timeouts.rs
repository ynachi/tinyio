#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Layer 3/5: per-operation timeouts. Each test states its prediction first.
//! The wheel's slots are 64 ms wide, so a timeout may fire up to one slot late.

mod common;

use std::cell::{Cell, RefCell};
use std::io::Write;
use std::os::unix::net::UnixStream;
use std::rc::Rc;
use std::thread;
use std::time::{Duration, Instant};

use common::watchdog;
use tinyio::{Io, Options, is_cancelled};

const MS: Duration = Duration::from_millis(1);
/// One wheel slot, plus scheduling slack.
const SLACK: Duration = Duration::from_millis(200);

fn io() -> Io {
    watchdog(60);
    Io::new(Options::default()).unwrap()
}

fn pending_pipe() -> (std::io::PipeReader, std::io::PipeWriter) {
    std::io::pipe().unwrap()
}

fn timed_out(e: &std::io::Error) -> bool {
    e.kind() == std::io::ErrorKind::TimedOut
}

// Prediction: a read on a pipe nobody writes fails with TimedOut, no earlier
// than the timeout and at most one slot (plus slack) after it.
#[test]
fn read_times_out_at_the_deadline() {
    let (r, _w) = pending_pipe();
    let mut io = io();
    let took = Rc::new(Cell::new(Duration::ZERO));
    let t = took.clone();
    io.spawn(move |cx| {
        let start = Instant::now();
        let e = cx
            .timeout(100 * MS)
            .read(&r, &mut [0u8; 1], None)
            .unwrap_err();
        assert!(timed_out(&e), "{e:?}");
        t.set(start.elapsed());
    })
    .unwrap();
    io.run_until_done();
    assert!(took.get() >= 100 * MS, "{:?}", took.get());
    assert!(took.get() < 100 * MS + SLACK, "{:?}", took.get());
}

// Prediction: a read that completes before its deadline succeeds, leaves
// nothing armed (the fiber finishes without the armed-timer abort), and the same
// fiber can go on to use the Io again.
#[test]
fn read_that_finishes_in_time_leaves_nothing_armed() {
    let (r, mut w) = pending_pipe();
    let mut io = io();
    let got = Rc::new(Cell::new(0));
    let g = got.clone();
    io.spawn(move |cx| {
        let n = cx
            .timeout(5_000 * MS)
            .read(&r, &mut [0u8; 8], None)
            .unwrap();
        g.set(n);
        cx.sleep(MS).unwrap();
    })
    .unwrap();
    io.spawn(move |cx| {
        cx.sleep(20 * MS).unwrap();
        w.write_all(b"abc").unwrap();
    })
    .unwrap();
    let started = Instant::now();
    io.run_until_done();
    assert_eq!(got.get(), 3);
    assert!(
        started.elapsed() < Duration::from_secs(2),
        "waited for the 5 s timer"
    );
}

// Prediction (plan, section 8): two fibers read one socket with the same
// timeout and one byte arrives just before it: one gets the byte, the other
// gets TimedOut, up to one slot late.
#[test]
fn data_just_before_the_deadline_goes_to_one_reader() {
    let (a, b) = UnixStream::pair().unwrap();
    let (a, b) = (Rc::new(a), Rc::new(b)); // `b` stays open, or the second reader sees EOF
    let writer_end = b.clone();
    let mut io = io();
    let outcomes = Rc::new(RefCell::new(Vec::new()));
    for _ in 0..2 {
        let (a, o) = (a.clone(), outcomes.clone());
        io.spawn(move |cx| {
            let r = cx.timeout(300 * MS).read(&*a, &mut [0u8; 1], None);
            o.borrow_mut().push(match r {
                Ok(n) => format!("data:{n}"),
                Err(e) if timed_out(&e) => "timeout".to_string(),
                Err(e) => format!("error:{e}"),
            });
        })
        .unwrap();
    }
    io.spawn(move |cx| {
        cx.sleep(270 * MS).unwrap();
        (&*writer_end).write_all(b"x").unwrap();
    })
    .unwrap();
    io.run_until_done();
    drop(b);
    let mut o = outcomes.borrow().clone();
    o.sort();
    assert_eq!(o, ["data:1", "timeout"]);
}

// Prediction: a write into a full pipe times out too.
#[test]
fn write_times_out() {
    let (_r, w) = pending_pipe();
    let mut io = io();
    let ok = Rc::new(Cell::new(false));
    let o = ok.clone();
    io.spawn(move |cx| {
        let chunk = vec![0u8; 64 * 1024];
        loop {
            match cx.timeout(80 * MS).write(&w, &chunk, None) {
                Ok(_) => {}
                Err(e) => {
                    o.set(timed_out(&e));
                    return;
                }
            }
        }
    })
    .unwrap();
    io.run_until_done();
    assert!(ok.get());
}

// Prediction: a timed-out fiber can immediately start another op, and it works.
#[test]
fn a_fiber_can_carry_on_after_a_timeout() {
    let (r1, _w1) = pending_pipe();
    let (r2, mut w2) = pending_pipe();
    let mut io = io();
    let got = Rc::new(RefCell::new(Vec::new()));
    let g = got.clone();
    io.spawn(move |cx| {
        let e = cx
            .timeout(40 * MS)
            .read(&r1, &mut [0u8; 1], None)
            .unwrap_err();
        g.borrow_mut()
            .push(if timed_out(&e) { "timeout" } else { "other" });
        let mut buf = [0u8; 4];
        let n = cx.read(&r2, &mut buf, None).unwrap();
        g.borrow_mut().push(if &buf[..n] == b"data" {
            "data"
        } else {
            "garbled"
        });
    })
    .unwrap();
    io.spawn(move |cx| {
        cx.sleep(250 * MS).unwrap();
        w2.write_all(b"data").unwrap();
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(*got.borrow(), ["timeout", "data"]);
}

// Prediction (late-cancel, plan section 8; invariant 7): the cancel for the
// timed-out op reaches the kernel before the fiber's next op can reuse its
// user_data, so that second op completes with data instead of being cancelled.
#[test]
fn the_cancel_cannot_hit_the_next_op() {
    let (r1, _w1) = pending_pipe();
    let (r2, mut w2) = pending_pipe();
    let mut io = io();
    let result = Rc::new(RefCell::new(String::new()));
    let res = result.clone();
    io.spawn(move |cx| {
        let _ = cx.timeout(30 * MS).read(&r1, &mut [0u8; 1], None);
        let mut buf = [0u8; 4];
        *res.borrow_mut() = match cx.read(&r2, &mut buf, None) {
            Ok(n) => String::from_utf8_lossy(&buf[..n]).into_owned(),
            Err(e) => format!("error: {e}"),
        };
    })
    .unwrap();
    io.spawn(move |cx| {
        cx.sleep(300 * MS).unwrap();
        w2.write_all(b"ok").unwrap();
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(*result.borrow(), "ok");
}

// Prediction (invariant 11): a timed op that `stop` interrupts fails with
// ECANCELED, not TimedOut, even though it had a deadline.
#[test]
fn stop_interrupting_a_timed_op_is_cancelled_not_timed_out() {
    let (r, _w) = pending_pipe();
    let mut io = io();
    let outcome = Rc::new(RefCell::new(String::new()));
    let o = outcome.clone();
    io.spawn(move |cx| {
        let e = cx
            .timeout(Duration::from_secs(60))
            .read(&r, &mut [0u8; 1], None)
            .unwrap_err();
        *o.borrow_mut() = if is_cancelled(&e) {
            "cancelled".into()
        } else {
            format!("{e:?}")
        };
    })
    .unwrap();
    let remote = io.remote();
    let t = thread::spawn(move || {
        thread::sleep(40 * MS);
        remote.stop();
    });
    let started = Instant::now();
    io.run();
    t.join().unwrap();
    assert_eq!(*outcome.borrow(), "cancelled");
    assert!(started.elapsed() < Duration::from_secs(2));
}

// Prediction: several timed ops with different deadlines fail in deadline order.
#[test]
fn deadlines_fire_in_order() {
    let mut io = io();
    let order = Rc::new(RefCell::new(Vec::new()));
    let mut pipes = Vec::new();
    for ms in [400u64, 130, 260] {
        let (r, w) = pending_pipe();
        pipes.push(w);
        let o = order.clone();
        io.spawn(move |cx| {
            let e = cx
                .timeout(Duration::from_millis(ms))
                .read(&r, &mut [0u8; 1], None)
                .unwrap_err();
            assert!(timed_out(&e));
            o.borrow_mut().push(ms);
        })
        .unwrap();
    }
    io.run_until_done();
    assert_eq!(*order.borrow(), [130, 260, 400]);
}

// Prediction: a zero timeout expires on the first pass of the loop (within a slot).
#[test]
fn zero_timeout_expires_at_once() {
    let (r, _w) = pending_pipe();
    let mut io = io();
    let took = Rc::new(Cell::new(Duration::MAX));
    let t = took.clone();
    io.spawn(move |cx| {
        let start = Instant::now();
        let e = cx
            .timeout(Duration::ZERO)
            .read(&r, &mut [0u8; 1], None)
            .unwrap_err();
        assert!(timed_out(&e));
        t.set(start.elapsed());
    })
    .unwrap();
    io.run_until_done();
    assert!(took.get() < SLACK, "{:?}", took.get());
}

// Prediction: 500 timed reads that all time out are all reported, with the
// slab and the lists coping with collisions in the same slot.
#[test]
fn many_timeouts_all_fire() {
    let mut io = io();
    let fired = Rc::new(Cell::new(0));
    let mut pipes = Vec::new();
    for i in 0..500u64 {
        let (r, w) = pending_pipe();
        pipes.push(w);
        let f = fired.clone();
        io.spawn(move |cx| {
            let e = cx
                .timeout(Duration::from_millis(20 + i % 150))
                .read(&r, &mut [0u8; 1], None)
                .unwrap_err();
            assert!(timed_out(&e));
            f.set(f.get() + 1);
        })
        .unwrap();
    }
    io.run_until_done();
    assert_eq!(fired.get(), 500);
}

// Prediction: the deadline counts from when the operation is armed, not from
// when the tick's clock was first read. Fiber A arms a 5 ms timeout at the start
// of the tick (which reads the clock, and makes the loop wake at once); fiber B then computes for 400 ms and arms a
// 300 ms timeout. B still gets the full 300 ms: data written 200 ms after its
// read started arrives in time. (With the tick's clock reading reused, B's
// deadline was already past when armed and its read failed at once.)
#[test]
fn deadline_counts_from_arming_not_from_the_tick_start() {
    let (r_a, w_a) = pending_pipe();
    let (r_b, mut w_b) = pending_pipe();
    let mut io = io();
    let outcome = Rc::new(RefCell::new(None));
    let o = outcome.clone();
    io.spawn(move |cx| {
        let _ = cx.timeout(5 * MS).read(&r_a, &mut [0u8; 1], None);
    })
    .unwrap();
    io.spawn(move |cx| {
        thread::sleep(400 * MS); // a long stretch of work inside one tick
        let mut buf = [0u8; 1];
        *o.borrow_mut() = Some(
            cx.timeout(300 * MS)
                .read(&r_b, &mut buf, None)
                .map(|_| buf[0]),
        );
    })
    .unwrap();
    let writer = thread::spawn(move || {
        thread::sleep(600 * MS);
        let _ = w_b.write_all(b"x"); // EPIPE if the read already gave up
        (w_a, w_b)
    });
    io.run_until_done();
    let _keep = writer.join().unwrap();
    let got = outcome.borrow_mut().take().unwrap();
    assert_eq!(got.unwrap(), b'x');
}
