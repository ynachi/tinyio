#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Layer 3: I/O through the ring. Files in a temp dir, pipes and socketpairs.
//! Each test states its prediction first. Needs Linux 6.1+.

mod common;

use std::cell::{Cell, RefCell};
use std::io::Write;
use std::os::unix::net::UnixStream;
use std::rc::Rc;
use std::time::{Duration, Instant};

use common::{TmpDir, watchdog};
use tinyio::{Io, OpenOptions, Options, is_cancelled};

fn io() -> Io {
    watchdog(60);
    Io::new(Options::default()).unwrap()
}

fn rw_create() -> OpenOptions {
    let mut o = OpenOptions::new();
    o.read(true).write(true).create(true).truncate(true);
    o
}

// Prediction: writes at explicit offsets land where asked, the file position is
// untouched by them, and a read at offset 6 sees only what was written there.
#[test]
fn file_write_and_read_with_offsets() {
    let dir = TmpDir::new("offsets");
    let path = dir.join("f");
    let mut io = io();
    let seen = Rc::new(RefCell::new(Vec::new()));
    let s = seen.clone();
    let p = path.clone();
    io.spawn(move |cx| {
        let fd = cx.open(&p, &rw_create()).unwrap();
        assert_eq!(cx.write(&fd, b"hello world", Some(0)).unwrap(), 11);
        assert_eq!(cx.write(&fd, b"RUST", Some(6)).unwrap(), 4);
        let mut buf = [0u8; 4];
        assert_eq!(cx.read(&fd, &mut buf, Some(6)).unwrap(), 4);
        s.borrow_mut().extend_from_slice(&buf);
        cx.close(fd).unwrap();
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(&*seen.borrow(), b"RUST");
    assert_eq!(std::fs::read(&path).unwrap(), b"hello RUSTd");
}

// Prediction: reading past what exists is a short read, then 0 at end of file.
#[test]
fn short_read_then_eof() {
    let dir = TmpDir::new("eof");
    let path = dir.join("f");
    std::fs::write(&path, b"12345").unwrap();
    let mut io = io();
    let got = Rc::new(RefCell::new(Vec::new()));
    let g = got.clone();
    io.spawn(move |cx| {
        let fd = cx.open(&path, OpenOptions::new().read(true)).unwrap();
        let mut buf = [0u8; 100];
        g.borrow_mut()
            .push(cx.read(&fd, &mut buf, Some(0)).unwrap());
        g.borrow_mut()
            .push(cx.read(&fd, &mut buf, Some(5)).unwrap());
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(*got.borrow(), [5, 0]);
}

// Prediction: with offset None, io_uring uses and advances the file position, like read(2).
#[test]
fn file_position_io() {
    let dir = TmpDir::new("pos");
    let path = dir.join("f");
    let mut io = io();
    let p = path.clone();
    io.spawn(move |cx| {
        let fd = cx.open(&p, &rw_create()).unwrap();
        cx.write(&fd, b"ab", None).unwrap();
        cx.write(&fd, b"cd", None).unwrap();
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(std::fs::read(&path).unwrap(), b"abcd");
}

// Prediction: opening a missing file fails with NotFound; an invalid option
// combination fails with EINVAL before reaching the kernel.
#[test]
fn open_errors() {
    let dir = TmpDir::new("open-err");
    let path = dir.join("missing");
    let mut io = io();
    let kinds = Rc::new(RefCell::new(Vec::new()));
    let k = kinds.clone();
    io.spawn(move |cx| {
        let e = cx.open(&path, OpenOptions::new().read(true)).unwrap_err();
        k.borrow_mut().push(e.kind());
        let e = cx.open(&path, &OpenOptions::new()).unwrap_err();
        k.borrow_mut().push(e.kind());
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(
        *kinds.borrow(),
        [
            std::io::ErrorKind::NotFound,
            std::io::ErrorKind::InvalidInput
        ]
    );
}

// Prediction: a NUL byte in a path is InvalidInput and never submitted.
#[test]
fn open_path_with_nul() {
    let mut io = io();
    let kind = Rc::new(Cell::new(None));
    let k = kind.clone();
    io.spawn(move |cx| {
        let e = cx.open("a\0b", OpenOptions::new().read(true)).unwrap_err();
        k.set(Some(e.kind()));
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(kind.get(), Some(std::io::ErrorKind::InvalidInput));
}

// Prediction: close consumes the descriptor; the data written before it is on disk.
#[test]
fn close_then_reopen() {
    let dir = TmpDir::new("close");
    let path = dir.join("f");
    let mut io = io();
    let out = Rc::new(RefCell::new(Vec::new()));
    let o = out.clone();
    io.spawn(move |cx| {
        let fd = cx.open(&path, &rw_create()).unwrap();
        cx.write(&fd, b"persist", Some(0)).unwrap();
        cx.close(fd).unwrap();
        let fd = cx.open(&path, OpenOptions::new().read(true)).unwrap();
        let mut buf = [0u8; 16];
        let n = cx.read(&fd, &mut buf, Some(0)).unwrap();
        o.borrow_mut().extend_from_slice(&buf[..n]);
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(&*out.borrow(), b"persist");
}

// Prediction: reading from the write end of a pipe fails with EBADF.
#[test]
fn bad_fd_is_ebadf() {
    let (_r, w) = std::io::pipe().unwrap();
    let mut io = io();
    let code = Rc::new(Cell::new(0));
    let c = code.clone();
    io.spawn(move |cx| {
        let mut buf = [0u8; 1];
        let e = cx.read(&w, &mut buf, None).unwrap_err();
        c.set(e.raw_os_error().unwrap());
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(code.get(), 9); // EBADF
}

// Prediction: a reader parks until the writer (which sleeps first) writes; the
// log reads reader-start, writer-write, reader-got. run_until_done does not
// return early while the read is in flight.
#[test]
fn pipe_reader_waits_for_writer() {
    let (mut r, mut w) = std::io::pipe().unwrap();
    let _ = &mut r;
    let mut io = io();
    let log = Rc::new(RefCell::new(Vec::new()));
    let l = log.clone();
    io.spawn(move |cx| {
        l.borrow_mut().push("reader-start");
        let mut buf = [0u8; 8];
        let n = cx.read(&r, &mut buf, None).unwrap();
        assert_eq!(&buf[..n], b"ping");
        l.borrow_mut().push("reader-got");
    })
    .unwrap();
    let l = log.clone();
    io.spawn(move |cx| {
        cx.sleep(Duration::from_millis(10)).unwrap();
        l.borrow_mut().push("writer-write");
        w.write_all(b"ping").unwrap();
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(
        *log.borrow(),
        ["reader-start", "writer-write", "reader-got"]
    );
}

// Prediction: two fibers ping-pong 200 round trips over a socketpair.
#[test]
fn socketpair_ping_pong() {
    let (a, b) = UnixStream::pair().unwrap();
    let mut io = io();
    let rounds = Rc::new(Cell::new(0));
    let r = rounds.clone();
    io.spawn(move |cx| {
        for i in 0..200u8 {
            cx.write(&a, &[i], None).unwrap();
            let mut buf = [0u8; 1];
            assert_eq!(cx.read(&a, &mut buf, None).unwrap(), 1);
            assert_eq!(buf[0], i.wrapping_add(1));
            r.set(r.get() + 1);
        }
    })
    .unwrap();
    io.spawn(move |cx| {
        for _ in 0..200 {
            let mut buf = [0u8; 1];
            assert_eq!(cx.read(&b, &mut buf, None).unwrap(), 1);
            cx.write(&b, &[buf[0].wrapping_add(1)], None).unwrap();
        }
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(rounds.get(), 200);
}

// Prediction: sleep lasts at least its duration, and a shorter sleeper started
// later finishes first.
#[test]
fn sleep_orders_by_deadline() {
    let mut io = io();
    let log = Rc::new(RefCell::new(Vec::new()));
    let started = Instant::now();
    for (name, ms) in [("slow", 60), ("fast", 15)] {
        let l = log.clone();
        io.spawn(move |cx| {
            cx.sleep(Duration::from_millis(ms)).unwrap();
            assert!(started.elapsed() >= Duration::from_millis(ms));
            l.borrow_mut().push(name);
        })
        .unwrap();
    }
    io.run_until_done();
    assert_eq!(*log.borrow(), ["fast", "slow"]);
}

// Prediction: sleep(0) returns at once without touching the ring.
#[test]
fn sleep_zero_is_immediate() {
    let mut io = io();
    let t = Rc::new(Cell::new(Duration::MAX));
    let tt = t.clone();
    io.spawn(move |cx| {
        let s = Instant::now();
        cx.sleep(Duration::ZERO).unwrap();
        tt.set(s.elapsed());
    })
    .unwrap();
    io.run_until_done();
    assert!(t.get() < Duration::from_millis(5));
}

// Prediction: a CQE that lands while other fibers keep the ready queue non-empty
// is reaped on the next tick, not after the yielder runs out. With a batch of 4
// fibers per tick the reader wakes within about two ticks of the write; if
// completions were held back (DEFER_TASKRUN only runs them on a GETEVENTS
// enter) the yielder would spin to its 100_000 limit first.
//
// Honest limit: io-uring 0.7.15's plain `submit()` also adds GETEVENTS whenever
// the TASKRUN flag is already visible, and in this single-threaded test it is,
// so this cannot tell `Ring::submit(true)` (explicit zero-timeout wait) from
// plain `submit()`; I checked by mutation. It guards the behaviour. The explicit
// form only matters when a completion races the syscall, which one thread
// cannot reproduce deterministically.
#[test]
fn completion_is_reaped_while_the_loop_stays_runnable() {
    watchdog(60);
    let (r, mut w) = std::io::pipe().unwrap();
    let mut io = Io::new(Options {
        batch_max_size: 4,
        ..Options::default()
    })
    .unwrap();
    let yields = Rc::new(Cell::new(0u32));
    let wrote_at = Rc::new(Cell::new(u32::MAX));
    let woke_at = Rc::new(Cell::new(u32::MAX));

    let (y, woke) = (yields.clone(), woke_at.clone());
    io.spawn(move |cx| {
        let mut buf = [0u8; 1];
        cx.read(&r, &mut buf, None).unwrap();
        woke.set(y.get());
    })
    .unwrap();

    let (y, wrote) = (yields.clone(), wrote_at.clone());
    io.spawn(move |cx| {
        // Let the read reach the kernel and arm, then write behind its back.
        for _ in 0..3 {
            cx.yield_now().unwrap();
        }
        wrote.set(y.get());
        w.write_all(b"x").unwrap();
    })
    .unwrap();

    let (y, woke) = (yields.clone(), woke_at.clone());
    io.spawn(move |cx| {
        while woke.get() == u32::MAX && y.get() < 100_000 {
            y.set(y.get() + 1);
            cx.yield_now().unwrap();
        }
    })
    .unwrap();

    io.run_until_done();
    assert_ne!(woke_at.get(), u32::MAX, "reader never woke");
    assert!(
        woke_at.get() <= wrote_at.get() + 12,
        "reader woke {} yields after the write: the CQE was held back",
        woke_at.get() - wrote_at.get()
    );
}

// Prediction (Rule 12): moving the Io between runs changes nothing, with I/O in flight in between.
#[test]
fn io_moves_between_runs_with_io() {
    let (r, mut w) = std::io::pipe().unwrap();
    let mut io = io();
    let got = Rc::new(Cell::new(0u8));
    let g = got.clone();
    io.spawn(move |cx| {
        let mut buf = [0u8; 1];
        cx.read(&r, &mut buf, None).unwrap();
        g.set(buf[0]);
    })
    .unwrap();
    io.spawn(move |cx| {
        cx.sleep(Duration::from_millis(5)).unwrap();
        w.write_all(&[7]).unwrap();
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(got.get(), 7);

    let mut moved = io; // the ring and the stacks stay where they are
    let g = got.clone();
    moved
        .spawn(move |cx| {
            cx.sleep(Duration::from_millis(1)).unwrap();
            g.set(9);
        })
        .unwrap();
    moved.run_until_done();
    assert_eq!(got.get(), 9);
}

// Prediction: 300 sleepers on a ring with 4 SQ slots (CQ 8) all finish: the SQ-full
// path submits and retries, and the CQ overflows into the kernel's list and is flushed.
#[test]
fn tiny_ring_survives_many_inflight_ops() {
    watchdog(60);
    let opts = Options {
        sq_entries: 4,
        ..Options::default()
    };
    let mut io = Io::new(opts).unwrap();
    let done = Rc::new(Cell::new(0));
    for i in 0..300u64 {
        let d = done.clone();
        io.spawn(move |cx| {
            cx.sleep(Duration::from_millis(1 + i % 7)).unwrap();
            d.set(d.get() + 1);
        })
        .unwrap();
    }
    io.run_until_done();
    assert_eq!(done.get(), 300);
}

// Prediction: a fiber can do I/O, finish, and its recycled stack does I/O again.
#[test]
fn recycled_fibers_do_io() {
    let mut io = io();
    let n = Rc::new(Cell::new(0));
    for _ in 0..5 {
        let c = n.clone();
        io.spawn(move |cx| {
            cx.sleep(Duration::from_millis(1)).unwrap();
            c.set(c.get() + 1);
        })
        .unwrap();
        io.run_until_done();
    }
    assert_eq!(n.get(), 5);
}

// Prediction: is_cancelled recognises only ECANCELED.
#[test]
fn is_cancelled_is_exact() {
    assert!(is_cancelled(&std::io::Error::from_raw_os_error(125)));
    assert!(!is_cancelled(&std::io::Error::from_raw_os_error(9)));
}
