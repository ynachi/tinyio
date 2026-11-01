#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Layer 2: fibers with no ring. Each test states its prediction first.

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use tinyio::{Io, Options};

fn io() -> Io {
    Io::new(Options::default()).unwrap()
}

// Prediction: a spawned closure runs exactly once, when the loop runs, not at spawn.
#[test]
fn spawn_runs_once_when_the_loop_runs() {
    let mut io = io();
    let n = Rc::new(Cell::new(0));
    let m = n.clone();
    io.spawn(move |_| m.set(m.get() + 1)).unwrap();
    assert_eq!(n.get(), 0);
    io.run_until_done();
    assert_eq!(n.get(), 1);
    io.run_until_done(); // nothing left: still 1
    assert_eq!(n.get(), 1);
}

// Prediction: two fibers that yield alternate strictly (a1 b1 a2 b2 a3 b3).
#[test]
fn yield_interleaves_in_fifo_order() {
    let mut io = io();
    let log = Rc::new(RefCell::new(Vec::new()));
    for name in ["a", "b"] {
        let log = log.clone();
        io.spawn(move |cx| {
            for i in 1..=3 {
                log.borrow_mut().push(format!("{name}{i}"));
                cx.yield_now().unwrap();
            }
        })
        .unwrap();
    }
    io.run_until_done();
    assert_eq!(*log.borrow(), ["a1", "b1", "a2", "b2", "a3", "b3"]);
}

// Prediction: ids are strictly increasing, and a recycled stack gets a fresh id.
#[test]
fn recycled_fibers_get_fresh_ids() {
    let mut io = io();
    let ids = Rc::new(RefCell::new(Vec::new()));
    for _ in 0..4 {
        let first = io.spawn(|_| {}).unwrap();
        io.run_until_done(); // the stack goes to the idle list
        ids.borrow_mut().push(first.get());
    }
    let v = ids.borrow();
    assert!(v.windows(2).all(|w| w[0] < w[1]), "{v:?}");
}

// Prediction: a fiber can spawn; the child runs after the parent next yields or ends.
#[test]
fn fibers_spawn_fibers() {
    let mut io = io();
    let log = Rc::new(RefCell::new(Vec::new()));
    let l = log.clone();
    io.spawn(move |cx| {
        l.borrow_mut().push("parent-start");
        let l2 = l.clone();
        cx.spawn(move |_| l2.borrow_mut().push("child")).unwrap();
        l.borrow_mut().push("parent-end");
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(*log.borrow(), ["parent-start", "parent-end", "child"]);
}

struct DropFlag(Rc<Cell<u32>>);
impl Drop for DropFlag {
    fn drop(&mut self) {
        self.0.set(self.0.get() + 1);
    }
}

// Prediction: a never-started fiber's closure is dropped (not run) when the Io drops.
#[test]
fn unstarted_closure_is_dropped_not_run() {
    let drops = Rc::new(Cell::new(0));
    let ran = Rc::new(Cell::new(false));
    {
        let io = io();
        let flag = DropFlag(drops.clone());
        let r = ran.clone();
        io.spawn(move |_| {
            let _keep = &flag;
            r.set(true);
        })
        .unwrap();
        assert_eq!(drops.get(), 0);
    }
    assert!(!ran.get());
    assert_eq!(drops.get(), 1);
}

// Prediction: a finished fiber's captures are dropped exactly once.
#[test]
fn started_closure_drops_captures_once() {
    let drops = Rc::new(Cell::new(0));
    let mut io = io();
    let flag = DropFlag(drops.clone());
    io.spawn(move |_| {
        let _keep = &flag;
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(drops.get(), 1);
    drop(io);
    assert_eq!(drops.get(), 1);
}

// Prediction: a 48 KiB capture lands on the 256 KiB fiber stack: a local of the
// fiber and the capture are within one stack's distance of each other.
#[test]
fn large_capture_lands_on_the_fiber_stack() {
    let mut io = io();
    let ok = Rc::new(Cell::new(false));
    let o = ok.clone();
    let big = [7u8; 48 * 1024];
    io.spawn(move |_| {
        let local = 0u8;
        let a = std::ptr::from_ref(&local) as usize;
        let b = big.as_ptr() as usize;
        let near = a.abs_diff(b) < 256 * 1024;
        o.set(near && big.iter().all(|&x| x == 7));
    })
    .unwrap();
    io.run_until_done();
    assert!(ok.get());
}

// Prediction: a closure over a quarter of the stack is refused with E2BIG (so
// it cannot leave the fiber too little stack to run), and the Io stays usable.
#[test]
fn oversized_closure_fails_cleanly() {
    let mut io = io();
    let huge = [0u8; 120 * 1024]; // > 64 KiB = a quarter of the 256 KiB stack
    let err = io
        .spawn(move |_| {
            let _ = &huge;
        })
        .unwrap_err();
    assert_eq!(err.raw_os_error(), Some(libc_e2big()));
    assert_eq!(err.kind(), std::io::ErrorKind::ArgumentListTooLong);
    let n = Rc::new(Cell::new(0));
    let m = n.clone();
    io.spawn(move |_| m.set(1)).unwrap();
    io.run_until_done();
    assert_eq!(n.get(), 1);
}

// Prediction: a closure just under the limit is accepted and runs.
#[test]
fn closure_under_the_limit_runs() {
    let mut io = io();
    let big = [7u8; 60 * 1024]; // < 64 KiB
    let sum = Rc::new(Cell::new(0usize));
    let s = sum.clone();
    io.spawn(move |_| s.set(big.iter().map(|&b| b as usize).sum()))
        .unwrap();
    io.run_until_done();
    assert_eq!(sum.get(), 7 * 60 * 1024);
}

// E2BIG on Linux; spelled out so the test needs no libc dependency.
fn libc_e2big() -> i32 {
    7
}

// Prediction: guard_size = 0 is rejected by validation (plan, section 8).
#[test]
fn zero_guard_is_rejected() {
    let opts = Options {
        guard_size: 0,
        ..Options::default()
    };
    let err = Io::new(opts).expect_err("must fail");
    assert_eq!(err.kind(), std::io::ErrorKind::InvalidInput);
}

// Prediction (Rule 12): moving the Io between runs changes nothing.
#[test]
fn io_can_be_moved_between_runs() {
    let mut io = io();
    let n = Rc::new(Cell::new(0));
    let m = n.clone();
    io.spawn(move |_| m.set(m.get() + 1)).unwrap();
    io.run_until_done();
    let mut io2 = io; // moved
    let m = n.clone();
    io2.spawn(move |_| m.set(m.get() + 1)).unwrap();
    io2.run_until_done();
    assert_eq!(n.get(), 2);
}

// Prediction: run, spawn, run again before any stop is supported (C++ DoubleRun).
#[test]
fn run_spawn_run_again() {
    let mut io = io();
    let n = Rc::new(Cell::new(0));
    for _ in 0..3 {
        let m = n.clone();
        io.spawn(move |_| m.set(m.get() + 1)).unwrap();
        io.run_until_done();
    }
    assert_eq!(n.get(), 3);
}

// Prediction: more fibers than max_idle_fibers all run; stacks beyond the cap are
// released (no leak, no double free) and the next burst works.
#[test]
fn idle_cap_releases_extra_stacks() {
    let opts = Options {
        max_idle_fibers: 4,
        ..Options::default()
    };
    let mut io = Io::new(opts).unwrap();
    let n = Rc::new(Cell::new(0));
    for _ in 0..2 {
        for _ in 0..500 {
            let m = n.clone();
            io.spawn(move |cx| {
                cx.yield_now().unwrap();
                m.set(m.get() + 1);
            })
            .unwrap();
        }
        io.run_until_done();
    }
    assert_eq!(n.get(), 1000);
}

// Prediction: aligned SSE/AVX-width data works on the fiber stack (the entry
// function sees RSP % 16 == 8 at its first instruction, as the ABI requires).
#[test]
fn aligned_vector_code_runs_on_a_fiber() {
    #[repr(align(32))]
    struct Aligned([f32; 8]);
    let mut io = io();
    let sum = Rc::new(Cell::new(0.0f32));
    let s = sum.clone();
    io.spawn(move |cx| {
        let mut a = Aligned([1.0; 8]);
        cx.yield_now().unwrap(); // registers and the aligned local survive a switch
        for (i, x) in a.0.iter_mut().enumerate() {
            *x += i as f32;
        }
        s.set(a.0.iter().sum());
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(sum.get(), 8.0 + 28.0);
}

// Prediction: catch_unwind works on a fiber stack (the trampoline's CFI marks
// the outermost frame; unwinding stops at our catch frame).
#[test]
fn catch_unwind_works_on_a_fiber_stack() {
    let mut io = io();
    let caught = Rc::new(Cell::new(0));
    let c = caught.clone();
    io.spawn(move |_| {
        let r = std::panic::catch_unwind(|| -> u8 { std::panic::resume_unwind(Box::new(41u32)) });
        if let Err(payload) = r {
            c.set(*payload.downcast::<u32>().unwrap() + 1);
        }
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(caught.get(), 42);
}

// Prediction: floating-point state survives switches: a fiber's f64 arithmetic
// across many yields is bit-identical to the same sum done without fibers.
#[test]
fn float_state_survives_switches() {
    let mut io = io();
    let got = Rc::new(Cell::new(0.0f64));
    let g = got.clone();
    io.spawn(move |cx| {
        let mut acc = 0.0f64;
        for i in 0..1000 {
            acc += (f64::from(i)).sqrt();
            cx.yield_now().unwrap();
        }
        g.set(acc);
    })
    .unwrap();
    io.spawn(|cx| {
        for _ in 0..1000 {
            cx.yield_now().unwrap();
        }
    })
    .unwrap();
    io.run_until_done();
    let want: f64 = (0..1000).map(|i| f64::from(i).sqrt()).sum();
    assert_eq!(got.get().to_bits(), want.to_bits());
}
