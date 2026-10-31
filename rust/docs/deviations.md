# Deviations from `architecture-plan.md`

Where the implementation departs from the plan, why, and the evidence. The plan
stays as written; this file is the errata.

## Stage 2

**1. The `GETEVENTS` submit is a plain enter, not a zero-timeout wait.**
The plan (section 6, "`GETEVENTS` and cancels") says a runnable tick should call
`submit_with_args(1, &SubmitArgs::new().timespec(&ZERO))` and treat `ETIME` as
success. Measured on Linux 6.18, one call each, `DEFER_TASKRUN` ring:

| call | ns/call |
|------|---------|
| `submit_with_args(1, zero timespec)` | 22,672 |
| `enter(len, 0, GETEVENTS)` (what liburing's `io_uring_submit_and_get_events` does) | 122 |
| `getpid()` | 101 |

Waiting for one CQE with a zero timeout is a real sleep/wake cycle. In the
context-switch bench it made a yield 108 ns instead of 15 ns (one tick per 128
resumes, so ~22 us per tick). `Ring::submit(true)` therefore calls the crate's
`Submitter::enter` (unsafe, inside `ring.rs`, which is in the unsafe budget) with
`min_complete = 0` and `IORING_ENTER_GETEVENTS`. Cost: one `unsafe` block.

`tests/io.rs::completion_is_reaped_while_the_loop_stays_runnable` guards the
behaviour (a CQE landing while other fibers stay ready is reaped promptly), but
it cannot distinguish this from the crate's plain `submit()`: `io-uring` 0.7.15's
`submit()` also adds `GETEVENTS` when the `TASKRUN` flag is already visible, and
in a single-threaded test it always is. The explicit form matters only when a
completion races the syscall.

**2. Drain cancel is `CancelBuilder::any().all()`.** The plan says `any()` is
enough. On Linux 6.18 it is (the kernel treats `ANY` as "all matches"; verified
with 3 pending reads and a timeout). `all()` is kept to mirror the C++
`ALL | ANY` and so the intent does not rest on that detail.

**3. The ring lives in `Sched`, not beside it.** `Cx` holds the scheduler handle
and nothing else (Rule 13), and `Cx::syscall` needs the ring, so `Sched` owns
`Ring`. It stays a separate type (`ring.rs`), opened by `Io::new`, closed by
`Io::drop` before the stacks are freed (never in `Sched::drop`).

**4. The ring is enabled in `Io::new`.** The plan enables it on the thread that
calls `run`. `Io` is `!Send`, so that is the creating thread, and `SINGLE_ISSUER`
binds to it either way.

**5. `ring.reap` copies CQEs to a stack batch and handles them after the borrow
of the `IoUring` has ended**, so the callback can never re-enter the ring (Rule 5
by construction rather than by care).

**6. `Cx::open` takes a `tinyio::OpenOptions`** (a mirror of `std::fs::OpenOptions`
with the same validation) instead of raw `libc::O_*` bits.

**7. `close` consumes the `OwnedFd`.** If the call is refused before reaching the
kernel (`ECANCELED`, `ENOSPC`) the fd is dropped, which closes it synchronously;
otherwise the kernel owns the close and the fd is forgotten.

**8. Miri runs with `-Zmiri-permissive-provenance`.** The plan requires
`expose_provenance` / `with_exposed_provenance` for `user_data`, which Miri's
strict mode rejects by definition.

## Stage 3

**9. `sync/mutex.rs` joins the unsafe budget.** `MutexGuard`'s `Deref`/`DerefMut` read an `UnsafeCell`, and the plan's table lists only `waiters.rs` and `scope.rs` under `sync/`; the gate script now allows it.

**10. Wait lists link through `FiberHeader::next`, not through nodes on parked stacks.** A `Waiting` fiber is in no other queue, so the link is free; this needs no per-wait node and no allocation. The ready queue and the wait lists share one type, `FiberQueue`.

**11. `Sched` is `pub` inside its private module** so the sealed `Attach` trait can name it without a private-in-public warning; it is not reachable from the crate root.

**12. `Sched::spawn_raw` (unsafe) backs `Scope::spawn`.** It is `spawn` without the `'static` bound, so no lifetime transmute is needed; `scope`'s join and its abort-on-unwind guard are what make it sound.

**13. `Semaphore::acquire`, `Latch::wait`, `Baton::wait` and `Mutex::lock` return `()`/a guard, not a `Result`,** because nothing can cancel a park (Rule 3). `Latch::count_down` past zero and a second concurrent `Baton::wait` abort.

**14. The `compile_fail` doctest for a child borrowing a local fails with E0373, not E0597.** Making `Scope`'s marker types covariant makes that doctest compile, so the invariant markers are what stop it.

**15. The stock C++ `sync_bench` has no baseline for its first three rows.** The compiler deletes those loops (0.32 ns per lock/unlock pair, `3.7e-06 ns/op` for latch create + count down), so the baseline is a scratch copy with an `asm volatile("" :: "r"(&x) : "memory")` barrier per iteration, which is not committed.

**16. Two uncontended operations are slower than C++.** Mutex lock/unlock 0.98 ns vs 0.53 (paired ratio 1.83, 0/11 rounds faster) and latch create + count down 2.86 ns vs 1.09 (ratio 2.62, 0/11); the causes are the cross-`Io` pointer check (Rule 13) and the `Rc<Sched>` increment/decrement (open decision 1), both kept on purpose. The other rows are ahead: semaphore 0.97 vs 2.54 ns, `scope` spawn + join 76.9 vs 100.6 ns (`TaskGroup`), contended mutex 30.9 vs 57.1 ns.

## Stage 4

**17. `Io::drop` cancels the eventfd read and waits for its CQE before closing the ring.** The read target lives in `Arc<Shared>`, and io_uring tears a ring down asynchronously, so the kernel could otherwise write into freed memory; `Ring` tracks the armed wake read (`wake_inflight`) and `close` aborts if it is still armed.

**18. `post_state` is never reset.** The shutdown subtracts exactly what it pops instead of storing `CLOSED`, so a producer that loses the push race and subtracts its own +1 later cannot underflow into the closed bit.

**19. `Options::post_queue_capacity` (default 4096)** mirrors the C++ option; `Remote::post` returns `Full` beyond it. `Io::new` creates the eventfd and `Shared` with the ring.

**20. Dispatch (open decision 4), measured with throwaway code outside the repo:** post to run, medians of 7 interleaved runs: 1 producer 797 ns vs C++ 762; 4 producers 531 ns vs C++ 3061; a zero-capture closure (no `Box` allocation) costs 765 ns, so the allocation is about 30 ns per post and small-buffer storage is not worth adding yet.

**21. `post` writes the eventfd only while the owner is asleep.** The owner raises a `sleeping` flag, fences, and re-checks for pending posts and stop before it blocks; a producer fences after its push and writes only if the flag is up (`stop` always writes). One-producer cost per post fell from 843 to 464 ns (C++ 701, paired ratio 0.70, faster in 14/15 rounds), and the blocking-tick fence made no detectable difference to ping-pong (stage 4 build vs now, 62 rounds: +4.3% and -2.5%).

**22. The producer-side fence cannot be exercised on x86**, because the locked compare-exchange and queue push before it already order the accesses; only the arm64 CI job covers it. The lock-step test in `ring.rs` fails on all three owner-side mutations I tried (no flag, no fence/re-check, re-check ignoring posts); the end-to-end race test in `tests/remote.rs` catches only a flag that is never raised.

## Stage 5

**23. The plan's late-cancel prediction does not reproduce.** With the immediate submit in `process_expired_timers` removed, `the_cancel_cannot_hit_the_next_op` still passed 5 of 5: the queued cancel is flushed by the next tick's submit, before the fiber can resume (only its own CQE wakes it). The immediate submit is kept (invariant 7) as defence in depth; it costs one syscall on a rare path.

**24. Timeout accuracy and cost.** The wait bound is the end of the nearest occupied 64 ms slot, so a timeout (a zero one included) fires up to one slot late; while stopping, expiry does nothing and the drain's cancel owns every op, so a stopped op is `ECANCELED`. A 30 s timeout on every op adds 245 ns per 4-op ping-pong round trip (C++: 335 ns); paired 0.961 against C++.

## Process note

A first attempt at checking item 1 by "mutation" changed nothing: `cargo fmt` had
reflowed the text the edit targeted, so the replace was a silent no-op. Mutation
checks now assert the edit applied (`grep -c`) before trusting the result.
