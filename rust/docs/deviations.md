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

**13. (Changed by the review fixes, item 32.) `Semaphore::acquire`, `Latch::wait`, `Baton::wait` and `Mutex::lock` now return `io::Result`** and are woken with `ECANCELED` by a stop. `Latch::count_down` past zero and a second concurrent `Baton::wait` still abort.

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

## Stage 6

**25. `net/sockopt.rs` joins the unsafe budget.** Socket creation, `setsockopt`, `bind` and `listen` need `libc`, and std cannot set `SO_REUSEPORT` or a backlog; the plan said `net/` calls only safe code. The address codec, listener and stream are `forbid(unsafe_code)`.

**26. Smaller than the C++ net layer, on purpose.** `SO_SNDBUF`/`SO_RCVBUF` setters are not ported, and the examples take `--name=value` flags and end on `--seconds` or an accept timeout instead of SIGINT handlers (no `unsafe` means no signal handler without a dependency).

**27. HTTP throughput is 2 to 4% behind C++, cause unknown (see item 46 for one small per-tick cost that was removed and did not settle it).** Measured with a throwaway load generator (32 keep-alive connections, 2 workers on 2 cores, load on the other 2, distinct port per run): Rust 215k and 222k req/s against C++ 222k and 224k, paired 0.962 and 0.976 over 2 x 15 rounds, faster in 5/15 each time. Not batching (about 0.13 `io_uring_enter` per request in both), and not cache aliasing of the fiber headers (stack colouring tested: 0.980 and 1.022, no effect, reverted).

**28. The IPv6 integration test skips where there is no IPv6 loopback** (this sandbox has none); the sockaddr codec's unit tests pin the v6 layout and length everywhere.

## Disk benches

**29. `Cx::fallocate` was added** (mode 0 only) because the C++ write bench preallocates with `posix_fallocate` and the Rust twin must do the same to measure the same filesystem path.

**30. Disk I/O matches C++ within this machine's noise.** Environment: a virtio disk in a microVM, no `fio` (so the control is a blocking `O_DIRECT` `pread` loop: 16.1k IOPS and 62 us at QD1, which is also what both runtimes get; 87.7k at QD32 and 128.7k at QD128 against about 200k for the io_uring runtimes). Paired, interleaved, one CPU, a freshly created file per arm: 64 KiB writes with 1 to 32 fibers, 9 rounds, medians of time per MiB 0.96 to 1.04 of C++; 4 KiB reads at QD 1/32/128, time per op 0.92 to 1.07 of C++ (15 rounds), individual ratios from 0.3 to 1.9. With the content check off in both, Rust is 0.94 to 0.97 on four of five scenarios.

**31. The first verified read runs wrongly showed Rust 9% slower at QD128** (21 rounds, 1.09): the Rust content check took about 1.4 us per 4 KiB against 0.7 us in C++ (per-word division and a slice compare). Rewritten as blocks, then words, with `u64` compares, it is level, and `--no-verify` on the Rust bench exists to separate the checker from the runtime. The write bench submits `Write` where the C++ one submits `Writev` with one iovec.

## Review fixes

An independent review of the crate (three reviewers, each reading it as ordinary Rust) found no way to cause undefined behaviour from safe code, and the following reliability and API problems. Each has a regression test; the first four were reproduced before they were fixed.

**32. Sync waits are cancellable.** `Remote::stop` used to abort the process if any fiber was parked on a `Mutex`, `Semaphore`, `Latch` or `Baton` (the producer it waited for had been cancelled). Parked lists are now registered with the scheduler (`WaitRegistry`, a `Vec` entry only on a list's 0 to 1 and 1 to 0 transitions), the shutdown wakes every waiter with `ECANCELED`, and a wait begun while stopping fails at once. A free lock is still taken while stopping. `Mutex::lock`, `Semaphore::acquire`, `Latch::wait` and `Baton::wait` return `io::Result`; the abort for a waiter nothing can wake remains for a stop-less `run_until_done` (a bug in the caller). Supersedes item 13 and invariant 10 of the plan for stopping runtimes.

**33. Timeouts count from when they are armed.** The tick's cached clock was reused by `arm_timeout`, so a fiber that ran for a while after another armed a timeout got a deadline that many milliseconds early (a 300 ms timeout failed at once after 400 ms of work). `arm_timeout` reads the clock itself (about 20 ns).

**34. A closure over a quarter of the stack is refused with `E2BIG`** (was half, `ENOMEM`): at half, a 120 KB capture on a 256 KB stack was accepted and the fiber then faulted on the guard page. The limit is documented on `Options::stack_size` and the `spawn` methods.

**35. `Cx::send` (`MSG_NOSIGNAL`) is new, and `TcpStream`'s `io::Write` uses it.** `write(2)` on a socket or pipe whose peer is gone raises `SIGPIPE` if the process leaves the signal at its default (Rust's runtime ignores it before `main`, a library hosted elsewhere may not). `Cx::write` keeps `write(2)` semantics and says so.

**36. A fiber's panic no longer aborts the process.** `fiber_entry` catches it (the one `catch_unwind` in the crate, which the gate now allows in `sched.rs`), stops the runtime, and `Io::run` / `run_until_done` resume the first payload after the drain. Panics inside tests' fibers now fail the test with its message instead of killing the binary. A panic through a `scope` with a live child still aborts (the child borrows the unwound frames).

**37. `TcpListener::bind` no longer sets `SO_REUSEPORT`.** With it a second bind of a port in use succeeded and a stale or foreign server could share it; `bind` now reports `AddrInUse` like std. `TcpListener::bind_reuse_port` is the opt-in, and the multi-worker examples use it.

**38. A posted closure that cannot get a fiber is kept, not dropped.** If the stack mapping fails while other fibers are live the closure waits (`stalled`) and is retried after a fiber finishes; with no live fiber to free a stack it is dropped and logged. Tested with a forced mapping failure.

**39. IPv6 `flowinfo` and `scope_id` are stored raw (native endian), as std does;** the port is the only big-endian field. The old code byte-swapped `flowinfo`, which a round-trip test cannot see.

**40. `Semaphore::acquire` returns a `Permit` (RAII, `forget` keeps it taken);** `release` adds a permit. `try_acquire` returns `Option<Permit>`. `MutexGuard`, `Permit` and `Timed` are `#[must_use]`; every public type has a `Debug`; `Mutex` has `get_mut` and `into_inner`.

**41. Timeout precision is documented as it is:** a deadline rounds up to the next 64 ms boundary of the runtime's clock, so a timeout never fires early but can be up to 64 ms late, and anything under 64 ms behaves like the next boundary (measured by the reviewer: 1, 5 and 10 ms all took 64 ms). `Cx::sleep` is exact. The tests now check "never early" and "within a slot plus 100 ms".

**42. Smaller changes.** `fiber_entry` takes its `Rc<Sched>` from a `Weak` the scheduler keeps of itself (two `unsafe` statements gone, and no pointer derived from `&Sched` bumps a count outside it); an expired timer whose fiber was already woken by a reap is skipped instead of cancelled; the 100k-round-trip lost-wakeup test shrank to 20k and yields after a short spin so a loaded one-CPU box no longer trips the 60 s watchdog; the examples back off on a failing `accept` instead of ending or spinning, set idle timeouts where they are not the benchmark, and `05_bench_io read` refuses `--mb=0`. Not changed: the single cancel-all in the drain has no retry (no reviewer could make an operation survive it), and the 64 KiB guard remains the only protection against a stack overflow.

**43. Cost of the review fixes, measured Rust against Rust** (the tree before the fixes against after, release builds, pinned to one CPU, rounds interleaved, paired): ping-pong 1.014 (15 rounds) and 1.001 with a 30 s timeout on every operation (so the clock read in `arm_timeout` is not visible); context switch level to +3% (noise between builds: the code is untouched); uncontended mutex 0.99, latch 0.95, contended mutex 0.92 (faster); **semaphore acquire + release 1.11** (about 0.25 ns: `acquire` now returns a `Result<Permit>`) and **`scope` spawn + join 1.07** (about 5 ns of 80: the panic-catching entry, the `Weak` upgrade and the reserve/launch split; each tried in isolation and none accounts for all of it). Both stay ahead of the C++ twins (semaphore and `TaskGroup` rows of item 16).

## Round C1

A second review (of 1595515) led to these; each has a test that predicts the behaviour, and the first two failed before the fix.

**44. `post` writes the eventfd once per sleep.** `Shared::post` now takes the `sleeping` flag down with a `swap` behind the plain load, so one producer per sleep pays the `write(2)` and the rest find the flag already down (a post to a running loop still costs one load). `a_burst_while_asleep_writes_the_eventfd_once` (4 threads x 1,000 posts into an announced sleep) saw 4,000 writes before the fix and exactly 1 after; `no_post_is_lost_with_two_racing_producers` is the lock-step guard for the new case (a producer whose load sees the flag up and whose swap sees it down). Mutations: a swap that does not write fails both lock-step tests (5 s timeout); a winner that writes only when `pending() <= 1` fails only the new two-producer test. Argument: the store-buffering pairing is untouched (owner: store flag, fence, load `pending`; producer: add to `pending`, push, fence, load flag), and a producer that loses the swap lost it to another producer (who writes) or to the owner's own `clear_sleep` (the owner is awake and its next `announce_sleep` fence pairs with ours). `clear_sleep` stays: the owner can wake for a CQE or a timer with the flag still up. A producer that clears the flag while `announce_sleep` is deciding not to block causes one spurious eventfd completion, which is harmless. Measured Rust against Rust, 4 CPUs, 100 bursts of 4 x 1,000 posts into an idle loop, 25 interleaved rounds: eventfd writes per burst 911 to 1.4 (`/proc/self/io` `syscw`); producer-side time per post 597 to 552 ns, paired 0.93 (faster in 16/25, min 0.49, max 1.76), **not the 2x the review expected: on this 4-vCPU VM the burst cost is mostly the queue's CAS contention, not the syscall**. The busy-loop `post` bench is unchanged within noise (1 producer 0.94, 4 producers 0.88, 15/25 each).

**45. A panicking scoped child no longer strands its parent.** The child's completion ran after `f(cx)`, so an unwind skipped it and the drain aborted with "1 on Scope". It now runs from a drop guard declared before the call: captures are still gone before the parent can wake, on the unwinding path too (`a_panicking_child_drops_its_captures_before_the_parent_wakes`). A panic in the parent with a live child still aborts, as before.

**46. `Ring::reap` no longer zeroes its 1 KiB batch.** The release assembly showed a 16-iteration store loop at the top of every `reap` (the `[(0u64, 0i32); 64]`), run on every tick whether or not a CQE was waiting. The batch is `MaybeUninit` now (one `unsafe` read of the first `n` slots, with the reason). Empty-CQ `reap`, release build, best of 30: **20.3 ns to 1.2 ns**. End to end the win is small and drowned by this VM's noise: ping-pong, three runs of 25 paired rounds, 0.966, 1.031 and 0.956; 03_http_services, 21 rounds, 1.057 (min 0.54, max 2.39). So this is a deterministic saving of about 19 ns per tick (about 1 to 2% of a 4.5 us round trip) that the end-to-end numbers cannot confirm or refute, and **it does not by itself explain, or close, the 2 to 4% HTTP gap of item 27**. The copy-out itself (only the first `n` entries are written and read) was left alone: handling CQEs in place would need `on_fiber` to be forbidden from reaping, which today it only is by convention.

**47. Two items settled without code.** (a) A posted closure dropped because no fiber stack can be mapped and no live fiber can give one back is a logged drop, now stated in `Remote::post`'s docs: it is out-of-memory, not a broken invariant, and an abort would turn memory pressure into a crash for a job the caller can already lose to shutdown. (b) A timeout cancel left in the SQ by a failed submit cannot hit the fiber's next operation, although both use the same `user_data`: the SQ is consumed in order, so the kernel runs the cancel before the later SQE exists. `a_queued_cancel_runs_before_a_later_op_with_the_same_user_data` pins that kernel fact (the cancel completes with `ENOENT`, the later read succeeds); the comment at the submit says so. The part of the review that read "stays queued" only applies when the submit fails after the cancel was queued: when `push_sqe` fails nothing is queued and the timer is re-armed a slot later.

**48. Smaller changes.** `TcpListener::accept(&self, cx)` returns a `TcpStream`; `From<TcpStream> for OwnedFd` lets a stream be closed through the ring with `Cx::close`; the crate refuses to compile off Linux x86_64/aarch64 with a message saying why (not exercised here: only the supported target's standard library is installed); `peer_addr`, `local_addr` and `shutdown` call `getpeername`, `getsockname` and `shutdown` directly in `net/sockopt.rs` instead of duplicating the descriptor for std (two syscalls fewer per call).

## Process note

A first attempt at checking item 1 by "mutation" changed nothing: `cargo fmt` had
reflowed the text the edit targeted, so the replace was a silent no-op. Mutation
checks now assert the edit applied (`grep -c`) before trusting the result.
