# zio Plan And Concepts

This document treats the current zio code as a prototype. The goal is to turn
the ideas that worked into a smaller, deliberate production core.

## Current Prototype Summary

The prototype has proven a few useful things:

- A fixed-pool stackful fiber scheduler can beat the simple baseline on HTTP.
- The minimal baseline still wins on narrow file I/O benchmarks.
- Cross-thread spawn works when the context is run with `run_blocking()`.
- Eventfd wakeups should not be paid for by finite `run()` workloads unless
  they need cross-thread scheduling.
- io_uring setup flags are workload-sensitive. A flag set that helps file I/O
  may hurt HTTP, and the reverse can also happen.

Do not keep layering features into the prototype forever. Freeze it as a
benchmark/reference and rebuild a cleaner production core.

## Core Concepts

### io_context

An `io_context` owns one `io_uring` and one scheduler loop.

The important rule is ownership: the scheduler thread owns the ring, the ready
queue, the timer queue, and the fiber pool. Other threads may request work, but
they should not directly mutate scheduler-owned structures.

That rule keeps the hot path simple and avoids locks around every operation.

### Fiber

A fiber is a stackful user-space task.

Unlike a coroutine, a stackful fiber has its own stack. Local variables survive
when the fiber suspends because the whole stack is preserved. This makes I/O
code look synchronous:

```cpp
auto n = ctx.recv(fd, buffer);
auto m = ctx.send(fd, response);
```

Each call submits one io_uring operation, parks only the current fiber, and
returns when the completion arrives. The OS thread continues running other
fibers.

### Boost.Context Fiber

`boost::context::fiber` is the public Boost.Context stackful context API.

Under the default implementation it is backed by `fcontext_t`, but zio should
prefer the public API unless a benchmark proves that direct use of
`boost::context::detail::fcontext_t` is worth the dependency on Boost internals.

At a high level:

- the scheduler resumes a fiber
- the fiber runs user code
- the fiber resumes the scheduler context when it blocks or finishes
- both sides receive the other side's latest continuation

This is why each `Fiber` stores both:

- its own suspended context
- the scheduler context it should return to

### Ready Queue

The ready queue contains fibers that can run immediately.

When a fiber calls `yield()`, it is put at the back of the ready queue. When an
I/O operation completes, the waiting fiber is also put back onto the ready queue.

The scheduler repeatedly drains ready fibers until they park, finish, or create
more work.

### I/O Wait

When a fiber performs I/O:

1. It gets an SQE from the ring.
2. It prepares the operation.
3. It stores a pointer in `user_data`.
4. It marks itself as waiting.
5. It jumps back to the scheduler.
6. The scheduler later sees the CQE.
7. The scheduler stores the result and requeues the fiber.
8. The fiber resumes and returns a `Result<T>`.

This is the core synchronous-looking API.

### Timers

Timers are fibers parked until a deadline.

The prototype uses an ordered timer set. A sleeping fiber stores an expiry time,
is inserted into the timer set, and yields to the scheduler. The scheduler wakes
expired timers before waiting for I/O.

Timers add bookkeeping to the scheduler loop, so the production core should
initially be built without timers, then timers should be added and benchmarked
as a separate feature.

### Cross-Thread Schedule

Cross-thread scheduling means a thread that does not own the `io_context` can
ask that context to run a new fiber.

The safe design is:

- remote thread allocates a small job node
- remote thread pushes it into an MPSC queue
- remote thread wakes the scheduler with `eventfd`
- scheduler thread drains the queue
- scheduler thread allocates a fiber from its own local pool
- the fiber starts on the scheduler thread

This preserves the invariant that fibers never migrate across threads.

### eventfd

`eventfd` is the wakeup mechanism for cross-thread scheduling.

When the scheduler is blocked in `io_uring_wait_cqe()`, another thread can write
to the eventfd. The scheduler has an eventfd read armed in the ring, so that
write completes as a CQE and wakes the scheduler.

Important distinction:

- `run()` should not pay eventfd overhead for finite local workloads.
- `run_blocking()` should arm eventfd because it is meant to stay alive for
  remote work.

### run()

`run()` means run until drained.

It should exit when:

- no ready fibers exist
- no timers exist
- no I/O is pending
- no remote jobs are queued

This mode is best for finite local work and benchmarks where all work is seeded
before the loop starts.

### run_blocking()

`run_blocking()` means keep the scheduler alive until stopped.

It is the right mode for:

- servers
- cross-thread `schedule()`
- externally posted work
- long-lived event loops

It should use eventfd wakeups and respect a stop token or `request_stop()`.

### Result<T>

zio should use `Result<T>` instead of exceptions in runtime paths.

Exceptions are acceptable during setup, where failures are one-shot and not on
the hot path. Runtime operations should return errors explicitly:

```cpp
zio::Result<std::size_t> n = ctx.recv(fd, buf);
```

This makes fiber code predictable and avoids exception propagation across
context-switch boundaries.

## What To Keep From The Prototype

Keep these choices:

- fixed fiber pool
- explicit `ctx.read/write/accept/recv/send`
- `zio::Result<T>`
- `spawn_local()` for owner-thread work
- `schedule()` for cross-thread work
- `run()` for finite local workloads
- `run_blocking()` for server/cross-thread workloads
- eventfd only for blocking/cross-thread mode

Avoid these choices:

- global TLS free-function API as the main API
- exception propagation through fibers
- heap allocation per fiber as the default
- one global io_uring flag set for every workload

## Workload Profiles

io_uring setup flags should be explicit and benchmarked by workload.

Recommended shape:

```cpp
enum class io_profile {
    plain,
    network,
    file,
};
```

Possible defaults:

```text
plain:
  flags = 0

network:
  flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN

file:
  flags = IORING_SETUP_SINGLE_ISSUER |
          IORING_SETUP_DEFER_TASKRUN |
          IORING_SETUP_COOP_TASKRUN
```

These are not final truths. They are starting points. The benchmark results so
far suggest:

- HTTP prefers no `DEFER_TASKRUN` in the zio prototype.
- File I/O may prefer `DEFER_TASKRUN`.
- The baseline does very well in file I/O because it is minimal and uses the
  defer/taskrun style effectively.

## Implementation Plan

### 1. Freeze The Prototype

Keep the current `zio/` prototype as a benchmark reference.

Record:

- source revision
- compiler
- build flags
- HTTP benchmark command and result
- file benchmark command and result
- failed experiments

The prototype is useful because it tells us what not to do as well as what to
keep.

### 2. Build A Minimal Core

Start a clean production core with only:

- fixed fiber pool
- ready queue
- free list
- `read`
- `write`
- `accept`
- `recv`
- `send`
- `run()`
- `Result<T>`

Do not include yet:

- timers
- cross-thread schedule
- eventfd
- logger
- socket helper classes
- cancellation

Benchmark this minimal core against the baseline. This gives a clean cost model
for zio's fixed pool and explicit API.

### 3. Add Features One At A Time

Add and benchmark in this order:

1. timers
2. `run_blocking()`
3. eventfd wake
4. cross-thread `schedule()`
5. shutdown/cancellation
6. socket helpers
7. logging hooks

After every feature, run the same tests:

```bash
oha -m GET http://127.0.0.1:8080/ -z 60s -c 1500
```

```bash
./zio_file_io_bench --path=/tmp/kio-zio.dat --bytes=1g --block=64k
```

Also run:

- cross-thread schedule smoke
- timer smoke
- pool exhaustion test

### 4. Define Shutdown Semantics

Production zio needs exact shutdown behavior.

Decide what happens when:

- `request_stop()` is called
- a fiber is waiting on I/O
- a fiber is sleeping
- remote jobs are queued
- new `schedule()` arrives after stop
- the context is destroyed with live work

Recommended behavior:

- `schedule()` after stop returns `operation_canceled`
- sleeping fibers wake with `operation_canceled`
- pending I/O is canceled if possible
- context destruction logs or asserts if live fibers remain
- `run_blocking()` drains cancellation work before returning

### 5. Add Tests Before More Optimization

Minimum tests:

- spawn N fibers and all complete
- nested spawn
- yield ordering
- sleep ordering
- TCP loopback accept/recv/send
- HTTP smoke
- cross-thread schedule from N producer threads
- schedule after stop returns canceled
- pool exhaustion returns `ENOMEM`
- `run()` exits when drained
- `run_blocking()` stays alive until stopped
- destroying an idle context is clean

### 6. Keep The Baseline

Keep `other_baseline` as a benchmark control.

Track ratios:

```text
zio_http_requests_per_sec / baseline_http_requests_per_sec
zio_file_mib_per_sec / baseline_file_mib_per_sec
```

Targets:

- HTTP: zio should be equal or faster.
- File I/O: zio should first get within 5-10%, then optimize from there.

### 7. Optimize Last

Only optimize after semantics and tests are stable.

Potential optimizations:

- switch Boost.Intrusive hooks from `safe_link` to `normal_link` in release
- reduce `Fiber` size
- tune stack size
- specialize network and file profiles
- multishot accept
- provided buffers for recv
- fixed file registration for file mode
- separate finite-run fast path if benchmarks prove it helps

Do not optimize by removing production semantics. The baseline is fast partly
because it does less. zio should aim to be fast while still being controllable.

## Lessons From Experiments

### Eventfd

Making eventfd conditional was a good idea. Finite `run()` workloads should not
pay for wake CQEs.

### DEFER_TASKRUN

Removing `IORING_SETUP_DEFER_TASKRUN` improved HTTP in one zio run but hurt file
I/O. This supports workload profiles instead of one global flag set.

### Baseline Hot Loop

Trying to transplant the baseline's simple `submit_and_wait()` run loop into zio
did not help. It reduced HTTP throughput and did not improve file I/O.

### Stack-Local I/O Operation

Trying a stack-local I/O operation record like the baseline also regressed in
zio. The idea works in the baseline's architecture, but it did not compose well
with zio's current scheduler shape.

Conclusion: borrow concepts carefully, benchmark after every change, and revert
changes that do not pay for themselves.
