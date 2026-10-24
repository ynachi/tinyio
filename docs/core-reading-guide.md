# Reading the tinyio core: fibers, the event loop and its invariants

Status: LIVING DOCUMENT. Started 2026-10-24, after the fiber-core review and
the kio-style shutdown fix (quiesce = nothing ready, nothing in flight;
parked fibers at quiesce abort).

Code references name functions, not line numbers: `runtime.cc` moves fast.

## The mental model

One OS thread owns one io_uring ring and many fibers. A fiber is a stack
plus a saved stack pointer. It runs until it submits an I/O operation, then
switches back to the scheduler. The scheduler reaps completions, marks fibers
ready and switches back into them. Everything else is bookkeeping around
that cycle.

The core is about 1,100 lines of implementation, split by concern into four
files, plus the headers they lean on:

| File | Lines | What lives there |
|------|-------|------------------|
| `runtime/runtime.cc` | 371 | construction, activation, the run loop, shutdown |
| `runtime/scheduler.cc` | 173 | fibers: spawn, resume, suspend, destroy, yield |
| `runtime/event_loop.cc` | 295 | the tick, submit/wait, reap, timers, the eventfd wakeup |
| `runtime/io_ops.cc` | 234 | the thin `Read`/`Write`/`Open`/`Accept`/… wrappers |

The class is still one `IO`: these are translation units, not layers, and a
single operation crosses several of them.

The four sit in `src/tinyio/runtime/`, next to `runtime.hpp` (the class
itself), `fiber_sync.hpp` (the sync primitives) and `buffer_pool.{hpp,cc}`.
The socket layer is in `src/tinyio/net/`; the mechanism — the context switch,
stacks, fibers, queues, the timer wheel — is in `src/tinyio/detail/`.

## Reading order

Follow one I/O call from top to bottom, one layer at a time. Check each layer
against the invariants below before moving on.

File names below are relative to `src/tinyio/`.

### 1. The context switch

`detail/context.hpp`, `detail/context.cc` (~233 lines).

- Draw the stack `Make()` builds, byte by byte, and check it against what
  `TinyioSwitchContext` pops (`InitialFrame`: 64 bytes on x86_64, 176 on
  aarch64).
- Step through the first switch into a new fiber in gdb with `si` and
  `info registers`: switch → `ret` → `TinyioContextTrampoline` →
  `IO::FiberEntry`. Once you have watched that, nothing in the fiber layer is
  mysterious.

### 2. The data structures

`detail/fiber.hpp`, `detail/stack.cc`.

- `Operation` is embedded in `Fiber`, and its address is the `user_data` the
  kernel hands back. That only works because of invariant 1.
- A stack is one `mmap`: a `PROT_NONE` guard page below a read/write region.

### 3. A fiber's lifecycle

`scheduler.cc`: `SpawnTask` → `ResumeFiber` → `FiberEntry` → `SuspendFiber` →
`FinishCurrentFiber` → `DestroyFiber`.

- Track `state` and `live_fibers_` through each step. `ready` means exactly
  "in the ready queue".
- `DestroyFiber` keeps finished fibers (and their stacks) in `idle_fibers_`
  for reuse, up to `IoOptions::max_idle_fibers`. They stay in `all_fibers_`.

### 4. One I/O call (the heart)

- `IO::Syscall()` (`runtime.hpp`): get an SQE, `user_data = &op`, mark the
  fiber waiting, `++inflight_ops_`, suspend.
- `IO::DrainCompletions()` (`event_loop.cc`): CQE → `op->cqe_res` →
  `PushReady(op->fiber)`.
- Back in `Syscall()` after `SuspendFiber` returns: map `cqe_res` to a
  `Result`.
- The buffers, iovecs, paths and addresses handed to the kernel live on the
  suspended fiber's stack. Stackful fibers keep the I/O API this simple.

### 5. The event loop

`IO::Tick()`, `IO::SubmitOrWait()` (`event_loop.cc`),
`IO::DriveToQuiescence()` (`runtime.cc`).

- One ordering: resume, then submit/wait, then reap. Decide exactly when
  the loop may block in the kernel.
- The quiesce check in `Tick()` is the most subtle line in the file. Be able
  to explain it from memory (invariant 9).

### 6. Cross-thread posting

`IO::Post()`, `detail/mpsc_queue.hpp`, `IO::Wake()`, `IO::ArmWakeRead()`,
`IO::DrainRemote()` (the last three in `event_loop.cc`).

- The only code another thread ever touches.
- The eventfd read tagged `k_wakeup_sentinel` wakes a sleeping loop.
  `wake_mutex_` keeps `Wake()` from writing to an eventfd the destructor has
  already closed.

### 7. Timeouts

`IO::Syscall()`'s `timeout` argument, `detail/timer_wheel.hpp`,
`IO::ProcessExpiredTimers()` (`event_loop.cc`).

- Arming a timeout is a vector push: no SQE, no kernel timer. The kernel is
  only involved when one fires: an ASYNC_CANCEL, whose `-ECANCELED` becomes
  ETIMEDOUT through `Operation::timed_out`.
- The loop's sleep is bounded by `TimerWheel::NsUntilNext()`.
- Note why the cancel is submitted immediately (invariant 7).

### 8. Sync primitives

`runtime/fiber_sync.hpp`: `Latch`, `Semaphore`, `Mutex`, `TaskGroup`.

- Built on two hooks only: `IO::ParkRunning()` and `IO::Unpark()`.
- `Mutex::Unlock` and `Semaphore::Release` hand ownership straight to the
  woken fiber (`held_` stays true).

### 9. Shutdown

`IO::Run()`, `IO::CancelAndDrain()`, `IO::~IO()`, `IO::AbortIfParkedFibers()`
(all in `runtime.cc`).

- Read this last: it is where every invariant is tested at once.
- `Run()` sets `stopping_`, cancels every in-flight fiber op, then drives
  `Tick()` until quiesced.

Peripheral, after you own the core: the fixed buffer pool.

## Invariants

Each one names where it is enforced and what breaks without it.

1. **A fiber has at most one operation in flight.**
   `Syscall()` asserts `state == running`, and the fiber stays
   `waiting` until its CQE arrives. Without it, a second op would overwrite
   the embedded `Operation`, and a CQE would land in the wrong op.

2. **An `Operation` outlives every kernel reference to it.**
   `user_data` is `&fiber->operation`, and the op's buffers live on the
   fiber's stack. `DestroyFiber` only runs on `done` fibers, and `~IO` runs
   `CancelAndDrain()` before `io_uring_queue_exit()`. Without it, the kernel
   writes into freed or recycled memory.

3. **`inflight_ops_` counts submissions that still owe a terminal CQE.**
   Incremented once per submission (`Syscall`, cancel SQEs in
   `CancelAndDrain` and `ProcessExpiredTimers`); decremented once on its CQE
   in `DrainCompletions()`. The eventfd read is deliberately not counted,
   since it is always armed. Without it,
   the loop quiesces with work outstanding, or never quiesces.

4. **`user_data` has three meanings.**
   `k_wakeup_sentinel`: the eventfd read. `k_fiber_cancel_sentinel`: a
   cancel SQE. Anything else: the `Operation*` embedded in the fiber that
   submitted it.

5. **A fiber is in the ready queue at most once, and never once done.**
   `PushReady()` skips a fiber already `ready` (ready means queued) and
   asserts it is not `done`; `DestroyFiber()` asserts. Without it, a fiber is resumed twice or after
   its stack is recycled.

6. **Only the owner thread touches the ring, fibers and queues**, except
   `Post()` and `Wake()`.
   `IORING_SETUP_SINGLE_ISSUER`; the ring is created `R_DISABLED` and enabled
   in `Activate()` on the thread that calls `Run()`, which also resets
   `owner_tid_`. `Syscall()` CHECK-fails off-thread or outside a fiber
   (`CheckCanSubmit()`): both are programming errors, so they abort rather
   than return an error a caller could ignore.

7. **A cancel is submitted before its target's `Operation` can be reused.**
   `ProcessExpiredTimers()` and `CancelAndDrain()` call `io_uring_submit()`
   right after preparing cancels. Cancels match by `user_data`, so a cancel
   left in the SQ could match the fiber's next op.

8. **The loop blocks in the kernel only when something can still wake it**:
   a CQE, the eventfd, or the nearest timer deadline. `SubmitOrWait(false)`
   is only reached with nothing runnable, and its wait is bounded by
   `TimerWheel::NsUntilNext()`.

9. **Quiesced means nothing ready, nothing in flight, nothing posted.**
   Live fibers do not count: a fiber parked on a sync primitive waits for no
   completion either. `Tick()` returns instead of sleeping when quiesced in
   drain mode; `RunUntilDone()` and `CancelAndDrain()` stop there. (Counting
   live fibers here was the shutdown hang.)

10. **The runtime never wakes a sync-parked fiber; only its primitive does.**
    `ParkOn()` parks without a cancellation path, and neither does
    `AcquireFixedBuffer()`: the runtime never closes its buffer pool. A fiber
    still live at quiesce is a deadlock, and `AbortIfParkedFibers()` aborts. (Force-waking
    at shutdown leaked mutex ownership and woke parents before children.)

11. **During a drain, `inflight_ops_` only goes down.**
    `stopping_` is set before `CancelAndDrain()`; `Syscall()` and `Yield()`
    then fail fast with ECANCELED without submitting. That makes the drain
    terminate. A fiber that ignores ECANCELED and retries forever spins in
    phase 1; nothing can save it.

## Predict, then run

Write down the outcome first, then run it in the container. A wrong
prediction marks the part you don't own yet. Turn each one into a test.

- A fiber calls `Yield()` in a loop and ignores the error. What happens at
  shutdown?
- Two fibers each `Read` the same socket with a 1 s timeout; data arrives at
  0.99 s. Who gets what?
- The listening fd is closed while a fiber is parked in `Accept()`.
- `Post()` is called from 8 threads while the owner is blocked in
  `submit_and_wait`.
- Remove the immediate `io_uring_submit()` from `ProcessExpiredTimers()`.
  Write the test that catches it.
- Set `guard_size` to 0 and recurse deeply inside a fiber.

## Habits

- Tests are the specification. Read `shutdown_cancellation_test.cc` and
  `fiber_sync_test.cc` next to the code, and add a test for every question
  you answer.
- Write a short design note for each of: the event loop, one I/O op's
  lifecycle, shutdown. kio's `design-notes/shutdown.md` is the template. If
  you can write them without looking at the code, you own it.

## Open findings from the review

Good first changes once the reading is done; each touches a different layer.

1. **ASan fiber hooks** (`detail/sanitizer_hooks.hpp`):
   `FinishCurrentFiber` should pass `nullptr` as `fake_stack_save` on the
   final switch, or every finished fiber leaks its fake stack (the `asan`
   preset enables `detect_stack_use_after_return`). `SuspendFiber` passes no
   destination stack, so ASan sees the scheduler stack as `[0,0)`: capture
   `bottom_old`/`size_old` from the first `__sanitizer_finish_switch_fiber` in
   `FiberEntry` and pass them back.
2. **Stack guard and fiber count**: a frame larger than the 4 KB guard can
   land in the neighbouring fiber's stack; build with
   `-fstack-clash-protection` or enlarge the guard. Each stack is two VMAs, so
   `vm.max_map_count` (65,530) caps a process at ~32K fibers.
3. **x86 shadow stack**: the switch assembly lives in a C++ file, so
   `-fcf-protection` marks the binary shadow-stack compatible while the
   switch breaks shadow stacks. Latent until SHSTK is enabled. Build
   `detail/context.cc` with `-fcf-protection=branch`, or move the asm to a `.S`.
4. Minor: no CFI on `TinyioSwitchContext`; idle fibers keep dirty stack
   pages (`MADV_FREE` on caching); `TimerWheel::NsUntilNext()` scans all 512
   slots on every sleep.
