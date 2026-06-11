# tinyio production roadmap

This roadmap is about turning the current small runtime into a production runtime without losing the clean core design.
The priority is correctness and a clear contract first, then performance.

---

## Status overview

| Phase                     | Status                      | Summary                                                                                      |
|---------------------------|-----------------------------|----------------------------------------------------------------------------------------------|
| 1 — Contract cleanup      | 🟡 ~70%                      | Invariants enforced in code, not yet documented. Shutdown states not an enum.                |
| 2 — Lifecycle safety      | ✅ Implemented, ❌ Untested | Stop/drain/cancel/post all work. Zero tests exist.                                           |
| 3 — Core hardening        | 🟡 Partial                   | Fiber pool with HWM ✅, batched CQE drain ✅. Stack pool not done.                           |
| 4 — Network surface       | ✅ Done                     | accept, read, write, close, connect, poll, writev, readv, fallocate, ftruncate, TcpListener. |
| 5 — Testing               | ❌ Not started              | No tinyio tests at all. Only bitcask has tests.                                              |
| 6 — Fiber sync primitives | ❌ Not started              | FiberLatch, FiberSemaphore, FiberMutex, TaskGroup.                                           |
| 7 — Performance           | 🟡 Partial                   | Fixed buffers ✅, preserve_none ✅, DEFER_TASKRUN ✅. Multishot not done.                    |
| 8 — API freeze            | ❌ Not started              | —                                                                                            |

### Context switch

- ✅ Unified assembly for both GCC and Clang via `preserve_none`.
- ✅ CET (`endbr64`) and BTI (`bti c`) landing pads for hardware CFI.
- ✅ ASAN and TSAN sanitizer hooks work with both compilers.
- Minimum compiler: Clang ≥ 18 / GCC ≥ 15 (x86-64), Clang ≥ 19 / GCC ≥ 16 (aarch64).

---

## Phase 1: contract cleanup

- Remove the fake liburing backend and make the contract explicit: Linux with real liburing only.
- Document the hard invariants:
    - one runtime per owner thread
    - no fiber migration
    - no IO outside a running fiber
    - one in-flight operation per fiber
    - defined `run()` return states
    - defined destruction rules
    - defined file descriptor ownership rules
    - explicit thread-safety boundaries
- Replace ambiguous shutdown behavior with explicit runtime states:
    - `Running`
    - `StopRequested`
    - `Draining`
    - `StoppedClean`
    - `StoppedWithLiveWork`
    - `Fatal`
- Ensure `request_stop()` cannot make `run()` return success while fibers or io_uring operations are still live.

## Phase 2: lifecycle safety

### Implemented

- ✅ Owner-thread stop semantics with drain behavior:
  - `run()` sets `stopping_ = true`, issues `IORING_OP_ASYNC_CANCEL` for every
    waiting fiber, then drives `tick()` until `inflight_ops_ == 0 &&
    ready_queue_.empty()`.  Resumed fibers see `ECANCELED` from `syscall()`
    (checked on entry while `stopping_ == true`) and are destroyed normally.
- ✅ Safe destructor: if `is_activated_ && !stopping_ && has_live_work`, the
  destructor issues the same cancel+drain sequence before releasing kernel
  resources.  Destroying from a non-owner thread with live work calls
  `std::abort()` — `IORING_SETUP_SINGLE_ISSUER` forbids cross-thread
  submission.
- ✅ Cross-thread `post()`:
  - `IO::post(fn)` enqueues `fn` into a lock-free MPSC queue
    and calls `wake()` to complete the pending kernel eventfd read, waking a
    sleeping owner thread.
  - `tick()` drains the queue after `drain_completions()` and spawns each
    dequeued task via `spawn_task()`.  All fiber creation stays on the owner
    thread; no ring access from remote threads.
  - `owner_tid_` is now fixed at construction time (`initialize()`) rather than
    at first `activate()` call.
- ✅ Cancellation for outstanding operations: both `run()` quiesce and the
  destructor quiesce issue `io_uring_prep_cancel` for all waiting fibers.
- ✅ Defined behavior for blocked ops during shutdown: all new `syscall()` calls
  return `ECANCELED` immediately when `stopping_ == true`; pending kernel ops
  are cancelled and their CQEs deliver `-ECANCELED` to the waiting fiber.

## Phase 3: core runtime hardening

- Add checked arithmetic for stack size and guard size alignment.
- ✅ Bounded fiber pool with high-water mark (`max_idle_fibers`) to reuse stacks.
- ✅ Batched CQE draining with `io_uring_for_each_cqe` + `io_uring_cq_advance`.
- Add owner-thread checks for every owner-only API.
- Stabilize the error policy and operation names.
- Add an optional internal log hook for fatal runtime errors.

## Phase 4: real network surface

- ✅ Done. The public API covers:
    - `accept` (with and without client address)
    - `read`, `write`, `readv`, `writev`
    - `read_fixed`, `write_fixed` (registered buffers)
    - `open`, `close`, `remove`, `rename`
    - `connect`, `poll`
    - `fsync`, `fallocate`, `ftruncate`
    - `sleep_for_ns`
    - `TcpListener::Bind` (convenience for socket setup)
- Keep protocol/framework code out of the runtime.

---

## Phase 5: testing

This is the highest-priority gap. No tinyio tests exist. The `tests/tinyio/` directory needs to be created.

Tests are organised into four groups. Each test has a rationale explaining
*what invariant it protects* and *what real failure it would catch*.

### Group A — Fiber lifecycle

These tests verify that the core fiber state machine (ready → running → waiting → ready → done) works correctly, and that fibers are created, recycled, and destroyed without leaks or corruption.

| Test                    | What to do                                                                                                                                              | Why                                                                                                                                                                                                                                   |
|-------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `spawn_and_complete`    | Spawn a single fiber that sets a flag, call `run_until_done()`, check the flag.                                                                         | Proves the minimal end-to-end path works: stack allocation, context switch to fiber, task execution, `finish_current_fiber`, context switch back, fiber destruction. If this fails, nothing else can work.                            |
| `spawn_many_sequential` | Spawn 1000 fibers one at a time via `run_until_done()` loops.                                                                                           | Exercises fiber recycling via `idle_fibers_` pool. Catches use-after-free on recycled `Fiber` objects, stale `context` state from a previous incarnation, and `live_fibers_` counter drift.                                           |
| `spawn_many_concurrent` | Spawn 500 fibers before calling `run_until_done()`. Each fiber increments an atomic counter. Verify counter == 500.                                     | Tests the ready queue under load. Catches corruption in the intrusive list (e.g., a fiber appearing in both `ready_queue_` and `idle_fibers_`), and verifies that `tick()` resumes all fibers, not just `batch_max_size` of them.     |
| `yield_round_robin`     | Spawn 3 fibers. Each appends its ID to a shared vector, then yields, repeating 3 times. Verify the interleaving is round-robin (`[1,2,3,1,2,3,1,2,3]`). | Proves `yield()` actually suspends the current fiber and puts it at the back of the ready queue (FIFO). Catches bugs where `push_ready` puts the fiber at the front, or where `suspend_fiber` doesn't correctly save/restore context. |
| `fiber_pool_hwm`        | Set `max_idle_fibers = 4`. Spawn 10 fibers, let them finish. Check that `idle_fibers_.size() <= 4` and that `all_fibers_` reflects the correct count.   | Verifies that `destroy_fiber` respects the high-water mark and actually calls `release_stack` + `delete` for excess fibers. Catches unbounded memory growth if the HWM check is broken.                                               |
| `nested_spawn`          | Inside a running fiber, call `io.spawn(inner_task)`. Let both complete via `run_until_done()`.                                                          | Proves that `spawn_task` works from within a fiber (not just from `main`). This is a common pattern (e.g., accept loop spawning handler fibers). Catches reentrancy issues in the scheduler.                                          |

### Group B — Shutdown and cancellation

These tests verify that the runtime shuts down cleanly under every scenario, without leaking kernel resources, hanging, or crashing. This is the most common source of production bugs in async runtimes.

| Test                             | What to do                                                                                                                                                                  | Why                                                                                                                                                                                                                                                                           |
|----------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `stop_token_cancels_waiting`     | Spawn a fiber that calls `sleep_for_ns(10s)`. From the main thread, fire the stop token after 50ms. Verify `run()` returns within 200ms and the fiber received `ECANCELED`. | Proves that `cancel_and_drain()` actually issues `IORING_OP_ASYNC_CANCEL` for waiting fibers and that the cancelled CQE wakes them with the right error. Catches deadlocks where the cancel SQE is never submitted, or where `drain_completions` doesn't process cancel CQEs. |
| `stop_token_cancels_ready`       | Spawn a fiber that yields in a loop forever. Fire stop token. Verify the fiber is drained.                                                                                  | Tests cancellation of fibers that are ready (not waiting on io_uring). The `syscall()` early-return on `stopping_` only helps waiting fibers; ready fibers need to be drained by `cancel_and_drain`'s `tick()` loop.                                                          |
| `destructor_cancels_live_work`   | Spawn a fiber that blocks on `sleep_for_ns(10s)`. Let the `IO` object go out of scope without calling `run()` to completion.                                                | Proves the destructor's quiesce path works. Without it, `io_uring_queue_exit` would free the ring while the kernel still references `Operation*` pointers → kernel UAF → crash or silent corruption.                                                                          |
| `destructor_aborts_wrong_thread` | Construct `IO` on thread A. From thread B (with live fibers), let the `IO` go out of scope. Catch the abort signal.                                                         | `IORING_SETUP_SINGLE_ISSUER` forbids cross-thread submission. The destructor must abort rather than silently submitting cancel SQEs from the wrong thread. Catches missing `is_owner_thread()` check.                                                                         |
| `syscall_returns_ecanceled`      | Set `stopping_ = true` (via stop token), then from a still-running fiber call `io.read()`. Verify it returns `ECANCELED` without submitting an SQE.                         | Proves the early-exit guard in `syscall()`. Without it, new I/O operations would be submitted to the ring during shutdown, extending the drain indefinitely.                                                                                                                  |
| `double_run`                     | Call `run_until_done()` twice on the same `IO` instance (second call with new fibers).                                                                                      | Proves the runtime can be reused. Catches stale state from the first run (`stopping_`, `is_running_`, `inflight_ops_`) leaking into the second.                                                                                                                               |

### Group C — Cross-thread `post()`

These tests verify the MPSC queue + eventfd wakeup path. This is the only thread-safe API surface, so it must be exercised under real concurrency.

| Test                          | What to do                                                                                                                                          | Why                                                                                                                                                                                                              |
|-------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `post_before_run`             | Call `io.post(task)` before `run_until_done()`. Verify the task executes.                                                                           | Tasks posted before the event loop starts must not be silently dropped. Catches a bug where `tick()` only drains the post queue after receiving a CQE (which would never arrive if the queue is the only work).  |
| `post_wakes_sleeping_thread`  | Start `run()` on a thread with no fibers (so it sleeps in `submit_and_wait`). From another thread, `post(task)`. Verify the task runs within 100ms. | Proves the `wake()` → eventfd write → kernel completes the armed read SQE → `submit_and_wait` returns path. Catches bugs where `wake()` is not called, or where the eventfd read is not re-armed after a wakeup. |
| `post_concurrent_correctness` | 8 threads × 1000 `post()` calls. Each posted task increments an atomic counter. Verify counter == 8000.                                             | Stress-tests the MPSC queue under contention. Catches ABA problems, lost enqueues under CAS retries, and `try_dequeue_bulk` leaving items behind. Run under TSAN.                                                |
| `post_while_fibers_active`    | Spawn 10 fibers that each yield 100 times. While they run, 4 threads each post 100 tasks. Verify all 10 + 400 tasks complete.                       | Tests the interaction between the CQE drain path and the post-queue drain path in `tick()`. Catches a bug where `batch_max_size` throttling causes posted tasks to starve.                                       |
| `post_after_stop`             | Fire stop token, then call `io.post(task)`. Let the `IO` object destruct. Verify no crash and no leak (task's destructor runs).                     | The destructor drains the post queue and discards un-spawned tasks. This test catches a UAF where the task captures a reference to something that is already destroyed.                                          |
| `post_queue_full`             | Set MPSC capacity to 16. Post 17 tasks. Verify `post()` returns `false` for the 17th.                                                               | Proves the bounded queue actually rejects when full, and that the caller gets a clean failure signal.                                                                                                            |

### Group D — I/O correctness

These tests verify that the io_uring SQE submission → fiber suspend → CQE completion → fiber resume path works for real kernel I/O. They require a real kernel (no mocking).

| Test                        | What to do                                                                                                            | Why                                                                                                                                                                                                      |
|-----------------------------|-----------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `file_write_read_roundtrip` | Open a temp file, write 4KB, fsync, read it back, compare.                                                            | Proves the minimal I/O path: `open`, `write`, `fsync`, `read`, `close`. Catches wrong SQE prep, mangled `Operation` pointer in CQE `user_data`, or wrong `cqe_res` propagation.                          |
| `fixed_buffer_write_read`   | Register a buffer pool. Take a fixed buffer, write through it, read back via a fixed buffer, compare.                 | Exercises the `IORING_OP_WRITE_FIXED` / `READ_FIXED` path. Catches wrong buffer index in `io_uring_prep_*_fixed`, or buffer pool registration failures.                                                  |
| `tcp_accept_echo`           | Bind a TCP listener, spawn an accept loop that echoes. Connect from a client fiber, send data, read it back, compare. | End-to-end network test. Proves `TcpListener::Bind`, `accept`, `read`, `write`, `close` all work together. Catches wrong `sockaddr` handling, missing `SOCK_NONBLOCK`, or accept returning the wrong fd. |
| `tcp_many_clients`          | Same as above but with 100 concurrent client fibers.                                                                  | Stress-tests the scheduler + io_uring under concurrent I/O. Catches SQ exhaustion (the `get_sqe` → submit → retry path), CQ overflow, and intrusive-list corruption under load.                          |
| `partial_write`             | Write a 1MB buffer in a loop, checking return value and advancing.                                                    | Proves the caller can handle short writes. Catches assumptions that `write` always writes the full buffer. (This tests the user pattern, not a runtime bug, but it validates the API contract.)          |
| `sleep_for_accuracy`        | Call `sleep_for_ns(50ms)`, measure wall time. Verify it's between 40ms and 200ms.                                     | Proves the timeout SQE path works and that `ETIME` is correctly mapped to success. Catches a bug where `sleep_for_ns` returns an error instead of `Ok` on normal timeout expiry.                         |
| `connect_to_listener`       | Bind a listener, spawn a fiber that calls `connect()`, accept on the other side.                                      | Proves async connect works. Catches wrong `io_uring_prep_connect` args or missing error mapping.                                                                                                         |
| `remove_and_rename`         | Create two temp files, rename one, remove the other. Verify filesystem state.                                         | Exercises `IORING_OP_UNLINKAT` and `IORING_OP_RENAMEAT`. These are less common paths that are easy to get wrong (e.g., wrong `AT_FDCWD` usage).                                                          |

### Sanitizer matrix

Every test above must pass under all sanitizer modes. Each catches a different class of bug:

| Mode                      | What it catches                                                                 | Notes                                                                                                                                                                                |
|---------------------------|---------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Debug** (no sanitizers) | Logic errors, assertion failures                                                | Fast, run on every commit                                                                                                                                                            |
| **ASAN**                  | Heap/stack buffer overflows, use-after-free, double-free, stack-use-after-scope | The fiber stack switch uses `__sanitizer_start/finish_switch_fiber` hooks. If these are wrong, ASAN will either false-positive or miss real bugs.                                    |
| **TSAN**                  | Data races between threads                                                      | Critical for `post()` tests. The MPSC queue is the main cross-thread surface. TSAN must see the `__tsan_switch_to_fiber` hooks to understand that fiber switches are not data races. |
| **UBSAN**                 | Signed overflow, null deref, alignment, shift UB                                | Catches subtle issues in stack alignment arithmetic and pointer casts.                                                                                                               |
| **Valgrind**              | Memory leaks, use of uninitialised values                                       | Slower but catches leaks that sanitizers miss (e.g., leaked `mmap` regions from `allocate_stack`). Requires `VALGRIND_STACK_REGISTER`.                                               |

### Test infrastructure notes

- Tests need a real Linux kernel with io_uring (≥ 5.11, ideally 6.1+). They cannot run in a container without `/dev/io_uring` access or `CAP_SYS_ADMIN`.
- Use `tmpfile()` or a temporary directory for file I/O tests to avoid leaving debris.
- TCP tests should bind to `127.0.0.1:0` (port 0 = kernel picks a free port) to avoid port conflicts in CI.
- Timeout-based tests (`sleep_for_accuracy`, `stop_token_cancels_waiting`) need generous margins. CI machines are slow and overloaded. Use 5× the expected wall time as the upper bound.
- Keep the fast test suite under 10 seconds total. Move stress tests (many-client, saturation, long-running) into a separate `check-stress` target.

---

## Phase 6: fiber synchronization primitives

Build after the Phase 5 test suite is green. These primitives are all single-owner-thread structures — no atomics needed, because the fiber scheduler is the only thread that touches them. The wait list and ready queue are owned by the `IO` instance.

### Shared building block: wait list

Every primitive needs a way to park a fiber and wake it later. The pattern is the same across all of them:

```cpp
// Suspending side (inside a fiber):
//   1. Push current fiber* onto the primitive's wait list.
//   2. Call io.suspend_current_fiber() — this yields to the scheduler.
//   3. When resumed, the condition is already satisfied.

// Waking side (also on the owner thread):
//   1. Pop a fiber* from the wait list.
//   2. Call io.push_ready(fiber*) — scheduler will resume it on the next tick.
```

The internal `suspend_fiber` and `push_ready` hooks already exist in `runtime.cc`; the primitives just need access to them. The cleanest way is to expose a narrow `Scheduler` handle (or friend the primitives to `IO`).

---

### FiberLatch

Mirrors `std::latch`. A one-shot countdown: N fibers do work, one waiter proceeds when all are done.

```cpp
struct FiberLatch {
    explicit FiberLatch(IO& io, std::ptrdiff_t count);

    // Called by worker fibers. When count reaches 0, all waiters are pushed ready.
    void count_down(std::ptrdiff_t n = 1);

    // Suspends the calling fiber until count == 0. Returns immediately if already 0.
    void wait();
};
```

**Use case — parallel write then fsync:**
```cpp
FiberLatch latch(io, chunks.size());
for (auto& chunk : chunks)
    io.spawn([&] { io.write(fd, chunk); latch.count_down(); });
latch.wait();
io.fsync(fd);
```

**Implementation sketch:**
- `count_`: `std::ptrdiff_t`, decremented in `count_down`.
- `waiters_`: intrusive list of `Fiber*`.
- `count_down`: atomically decrement (or plain decrement since owner-thread only); if reaches 0, drain `waiters_` into the ready queue.
- `wait`: if `count_ > 0`, push self onto `waiters_`, call `suspend_current_fiber`.

---

### FiberSemaphore

A counting gate. Limits how many fibers may be in a critical section concurrently.

```cpp
struct FiberSemaphore {
    explicit FiberSemaphore(IO& io, std::ptrdiff_t initial);

    // Decrements count. Suspends if count == 0.
    void acquire();

    // Increments count. Wakes one waiter if any.
    void release();

    // Non-blocking attempt. Returns false instead of suspending.
    bool try_acquire();
};
```

**Use case — cap concurrent DB connections:**
```cpp
FiberSemaphore slots(io, 16);  // at most 16 concurrent queries
io.spawn([&] {
    slots.acquire();
    auto _ = defer([&] { slots.release(); });
    db.query(...);
});
```

**Implementation sketch:**
- `count_`: `std::ptrdiff_t`.
- `waiters_`: intrusive list of `Fiber*`.
- `acquire`: if `count_ > 0`, `--count_`; else push self to `waiters_`, suspend.
- `release`: if `waiters_` non-empty, pop front and push to ready queue (count stays 0); else `++count_`.

---

### FiberMutex

Mutual exclusion for shared state across fibers (not threads — use a real mutex for cross-thread protection).

```cpp
struct FiberMutex {
    explicit FiberMutex(IO& io);

    void lock();     // Suspends if already held.
    void unlock();   // Wakes next waiter, or marks free.
    bool try_lock(); // Non-blocking.

    // RAII guard
    auto scoped_lock() -> std::unique_ptr<Guard>;
};
```

**Use case — shared write buffer across handler fibers:**
```cpp
FiberMutex mu(io);
// in each handler fiber:
auto guard = mu.scoped_lock();
buffer.append(data);
// guard releases on scope exit → next waiter unblocked
```

**Implementation sketch:**
- `held_`: `bool`.
- `waiters_`: intrusive list of `Fiber*`.
- `lock`: if `!held_`, set `held_ = true`; else push self to `waiters_`, suspend.
- `unlock`: if `waiters_` non-empty, pop front and push to ready queue (do not clear `held_`); else `held_ = false`.
- Non-recursive. A fiber that double-locks will deadlock — document this explicitly.

---

### TaskGroup (structured concurrency)

Ties fiber lifetime to a lexical scope. Built directly on top of `FiberLatch`. Ensures that spawned child fibers cannot outlive the group.

```cpp
struct TaskGroup {
    explicit TaskGroup(IO& io);

    // Spawns a child fiber tracked by this group.
    void spawn(std::move_only_function<void()> task);

    // Suspends calling fiber until all spawned children have finished.
    // Must be called before the TaskGroup is destroyed.
    void wait();

    // ~TaskGroup asserts wait() was called (debug) or calls it (release).
};
```

**Use case — fan-out and collect:**
```cpp
io.spawn([&](IO& io) {
    TaskGroup group(io);
    group.spawn([&] { fetch(url_a); });
    group.spawn([&] { fetch(url_b); });
    group.wait();
    // both fetches done here, group is safe to destroy
});
```

**Implementation sketch:**
- Internally holds a `FiberLatch` initialised to 0 and incremented on each `spawn` (or use a counter + latch reset on `wait`).
- Each child task wraps the user function: run it, then call `latch.count_down()`.
- `wait()` calls `latch.wait()`.
- Error propagation: first child exception (or `std::expected` error) is stored atomically; `wait()` rethrows / returns it. All other children are let run to completion (cancel-on-first-error is a future extension).

---

### Build order

1. `FiberLatch` — simplest, self-contained, needed by TaskGroup.
2. `FiberSemaphore` — same pattern, independent.
3. `FiberMutex` — slightly more complex (ownership semantics).
4. `TaskGroup` — composed from `FiberLatch` + `spawn`.

---

## Phase 7: performance work

Do this only after the runtime contract is stable and the test suite is green.

- Tune stack pool size and stack size defaults.
- Add multishot accept (`IORING_ACCEPT_MULTISHOT`).
- Consider multishot recv after the basic socket contract is solid.
- ✅ Fixed buffers for registered I/O.
- ✅ `preserve_none` context switch (both GCC and Clang).
- ✅ `IORING_SETUP_DEFER_TASKRUN` for reduced kernel overhead.
- Add a multi-runtime `SO_REUSEPORT` example.
- Track scheduler cost per operation, not only end-to-end HTTP RPS.

## Phase 8: API freeze

- Freeze the public API shape.
- Write final invariants, shutdown, and cancellation docs.
- Decide whether `post()` is the only cross-thread API.
- Decide whether cancellation is explicit per operation or runtime-owned.
- Keep examples aligned with the final contract.
- Update the README (currently references the old "URing" name, `co_await`, and `Task<T>` which no longer exist).

---

## First implementation sprint

1. ✅ Remove the fake liburing backend.
2. ✅ Fix shutdown semantics (safe destructor quiesce, `stopping_` flag, cancellation).
3. ✅ Add cross-thread `post()` with eventfd wakeup.
4. ✅ Unified `preserve_none` context switch for GCC and Clang.
5. ✅ Batched CQE draining with `io_uring_for_each_cqe`.
6. ✅ Fiber recycling pool with high-water mark.
7. **Next: Write the Phase 5 test suite (Groups A–D above).**
8. Implement Phase 6 fiber sync primitives (FiberLatch → FiberSemaphore → FiberMutex → TaskGroup).
9. Re-benchmark against the current baseline.

## How to use tinyio error messages in logs

```c++
        // register pool if not empty
        if (buffer_pool_.bucket_count() != 0)
        {
            if (auto res = register_buffers(buffer_pool_); !res.has_value())
            {
                auto err = res.error();
                // We log the context (err.op) and the human-readable system error
                ALOG_ERROR("failed to register buffers: [{}] {}", err.op, err.error_code().message());
            }
            else
            {
                ALOG_INFO("the buffer pool has been registered to io_uring");
            }
        }

        ALOG_INFO("Initialized IO context with {} entries (SQPOLL: {})", opts_.entries,
                  (opts_.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");
```