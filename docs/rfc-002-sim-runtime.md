# RFC-002: SimRuntime — In-Memory Simulation Runtime for tinyio

**Status**: Draft
**Author**: tinyio contributors
**Created**: 2026-06-22
**Updated**: 2026-06-22

---

## Table of Contents

1. [Abstract](#1-abstract)
2. [Motivation](#2-motivation)
3. [Design Decisions Considered](#3-design-decisions-considered)
   - 3.1 [Simulation Depth](#31-simulation-depth)
   - 3.2 [SimIO Architecture Options](#32-simio-architecture-options)
   - 3.3 [Cross-Platform vs Sim-Only](#33-cross-platform-vs-sim-only)
4. [Chosen Design](#4-chosen-design)
5. [Component Breakdown](#5-component-breakdown)
   - 5.1 [Clock Abstraction](#51-clock-abstraction)
   - 5.2 [FiberScheduler Extraction](#52-fiberscheduler-extraction)
   - 5.3 [SimLoop](#53-simloop)
   - 5.4 [SimIO](#54-simio)
   - 5.5 [SimPipe](#55-simpipe)
   - 5.6 [SimFile](#56-simfile)
6. [Fault Injection Model](#6-fault-injection-model)
7. [Time Control](#7-time-control)
8. [File Layout](#8-file-layout)
9. [Impact on Existing Code](#9-impact-on-existing-code)
10. [Usage Examples](#10-usage-examples)
11. [Non-Goals](#11-non-goals)
12. [Open Questions](#12-open-questions)

---

## 1. Abstract

This RFC introduces `SimIO`, an in-memory simulation runtime for tinyio. `SimIO` exposes the same public API as the production `IO` class but replaces the io_uring backend with a fully controlled in-process simulation. It enables deterministic unit testing of fiber logic, protocol handlers, timeout behavior, and failure paths — without requiring io_uring, real network interfaces, or real disk.

The design also extracts `FiberScheduler` from `IO` into a standalone reusable component and introduces a `Clock` abstraction backed by `absl::SimulatedClock` in the simulation path, enabling instant time travel in tests without real sleeps.

---

## 2. Motivation

tinyio targets Linux io_uring for production. This creates friction for:

- **Unit testing**: tests require a real io_uring ring, real fds, real network. Setup is heavy and tests are slow or environment-dependent.
- **Fault injection**: inducing `ECONNRESET`, `EIO`, or partial writes against real kernel interfaces requires privileged tools (iptables, fault injection modules) or fragile wrapper scripts.
- **Timeout testing**: verifying that a fiber correctly handles a 30-second idle timeout requires waiting 30 seconds, or disabling the test entirely.
- **CI portability**: environments without io_uring support (restricted containers, older kernels) cannot run any tests today.

The goal is a `SimIO` that:

1. Compiles and runs on any Linux kernel (no io_uring required in test builds).
2. Provides virtual files and virtual sockets backed by in-memory buffers.
3. Allows injecting arbitrary errors at the transport level, not at individual call sites.
4. Allows advancing simulated time instantly to trigger timeouts.
5. Requires no changes to application fiber code — handlers written against `IO` work identically against `SimIO`.

---

## 3. Design Decisions Considered

### 3.1 Simulation Depth

Three levels of simulation were evaluated:

| Level | Description | Fault injection | Time control | Effort |
|-------|-------------|-----------------|--------------|--------|
| 1 | epoll + blocking POSIX, real I/O, no io_uring | Limited | No | Low |
| 2 | Injected results at the `IO` API call level | White-box only | Partial | Medium |
| 3 | Virtual transport (SimPipe, SimFile) with fault hooks | Transport-level | Full | High |

**Level 3 was chosen.** It is the only level where fault injection works transparently through retries and partial I/O, and it is the only level that enables instant time control. Levels 1 and 2 were rejected because Level 1 cannot inject faults and Level 2 requires white-box knowledge of which specific call fails.

### 3.2 SimIO Architecture Options

Three structural options were considered for how `SimIO` relates to `IO`:

**Option A — Shared virtual base class.**
Extract an `IoBase` abstract class. Both `IO` and `SimIO` inherit from it. User code takes `IoBase&`.

Rejected because: the `syscall<T>` template uses `io_uring_sqe*` directly. Abstracting the prep step requires a fat op-descriptor union or per-backend per-op methods, making the interface brittle. It also forces virtual dispatch on every I/O operation in production.

**Option B — Extracted `FiberScheduler`, composed into both `IO` and `SimIO`.**
The scheduler (fiber management, ready queue, timer wheel) moves into a standalone `FiberScheduler`. `IO` and `SimIO` each compose it alongside their respective event loop backends.

Chosen. See §4.

**Option C — Fully independent `SimIO`, duck-typed.**
`SimIO` is a standalone class with no shared code with `IO` beyond low-level primitives (`Fiber`, `TimerWheel`, `IntrusiveList`). User code is templated on the runtime type.

Rejected for primary use because it requires templating all user-facing handler code. However, the public API of `SimIO` will remain identical to `IO` so that duck-typing remains possible as a zero-overhead opt-in for performance-sensitive test harnesses.

### 3.3 Cross-Platform vs Sim-Only

macOS and other non-Linux platforms were evaluated as targets. Three approaches were considered:
- Full kqueue backend (native performance on macOS)
- Blocking POSIX shim (dev/test only on macOS)
- Sim-only (Linux stays the only production target; `SimIO` serves the dev/test use case)

**Sim-only was chosen.** tinyio is Linux-first. A kqueue backend adds significant maintenance surface for marginal benefit. `SimIO` already solves the core problem (developers can write and test fiber logic without io_uring), and does so better than a platform shim (fault injection, time control, no real I/O setup). macOS support is explicitly out of scope.

---

## 4. Chosen Design

```
                    FiberScheduler
                   (fibers, ready_queue,
                    timer_wheel, Clock*)
                         │
              ┌──────────┴──────────┐
              │                     │
             IO                   SimIO
    (FiberScheduler             (FiberScheduler
      + IoUringLoop)              + SimLoop)
                                     │
                              ┌──────┴──────┐
                           SimPipe       SimFile
                        (in-memory      (in-memory
                         socket pair)    byte buffer)
```

`FiberScheduler` owns:
- All fiber lifecycle management (`spawn`, `destroy`, `park`, `unpark`)
- The ready queue and idle fiber cache
- The timer wheel
- A `Clock*` for time queries (real or simulated)

`IO` owns `FiberScheduler` + the io_uring ring. Its `tick()` calls the io_uring submit/drain path unchanged.

`SimIO` owns `FiberScheduler` + a `SimLoop`. Its `tick()` drains the `SimLoop`'s completion queue instead of CQEs. `SimPipe` and `SimFile` post completions directly to `SimLoop`.

---

## 5. Component Breakdown

### 5.1 Clock Abstraction

**File**: `include/tinyio/detail/clock.hpp`

```cpp
namespace tinyio::detail {

struct Clock {
    virtual uint64_t now_ns() const noexcept = 0;
    virtual ~Clock() = default;
};

/// Production clock: wraps CLOCK_MONOTONIC.
struct RealClock final : Clock {
    uint64_t now_ns() const noexcept override {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
             + static_cast<uint64_t>(ts.tv_nsec);
    }
};

/// Simulation clock: wraps absl::SimulatedClock.
/// Time only advances when the test calls SimIO::advance_time().
struct AbslSimClock final : Clock {
    explicit AbslSimClock(absl::SimulatedClock& c) : clock_(c) {}
    uint64_t now_ns() const noexcept override {
        return static_cast<uint64_t>(absl::ToUnixNanos(clock_.TimeNow()));
    }
private:
    absl::SimulatedClock& clock_;
};

} // namespace tinyio::detail
```

`monotonic_now_ns()` is retained as a free function for any remaining call sites outside `FiberScheduler` but is now implemented via `RealClock`.

The three call sites in `runtime.cc` that currently call `detail::monotonic_now_ns()` directly become `scheduler_.clock_->now_ns()`. The timer wheel itself is unchanged — it already takes `now_ns` as a parameter everywhere.

### 5.2 FiberScheduler Extraction

**Files**: `include/tinyio/detail/scheduler.hpp`, `src/tinyio/scheduler.cc`

`FiberScheduler` is extracted from `IO` and contains exactly the members that have no io_uring coupling:

```cpp
namespace tinyio::detail {

class FiberScheduler {
public:
    explicit FiberScheduler(Clock* clock, const IoOptions& opts);

    // Fiber lifecycle
    Result<FiberId> spawn(std::move_only_function<void(...)> task) noexcept;
    void            push_ready(fiber::Fiber* f) noexcept;
    void            remove_ready(fiber::Fiber* f) noexcept;
    fiber::Fiber*   pop_ready() noexcept;
    void            park_running() noexcept;
    void            unpark(fiber::Fiber* f) noexcept;
    fiber::Fiber*   running_fiber() const noexcept;
    bool            has_live_fibers() const noexcept;

    // Timer
    void            process_expired_timers(CancelFn) noexcept;
    TimerWheel&     timer_wheel() noexcept;

    // Batch execution
    void            resume_ready_fibers() noexcept;
    void            drain_remote() noexcept;

    // Teardown
    void            destroy_all_fibers() noexcept;

    Clock* clock_;   // non-owning; set at construction

private:
    // All fiber management state currently in IO:
    IntrusiveList<fiber::Fiber, AllFibersTag,  false> all_fibers_;
    IntrusiveList<fiber::Fiber, ReadyQueueTag, false> ready_queue_;
    IntrusiveList<fiber::Fiber, IdleQueueTag,  true>  idle_fibers_;
    fiber::Fiber*   current_fiber_ = nullptr;
    FiberId         next_fiber_id_ = 1;
    std::size_t     live_fibers_   = 0;
    TimerWheel      timer_wheel_;
    IoOptions       opts_;
    // cross-thread post queue
    MpscQueue<std::move_only_function<void(...)>> post_queue_;
    // ...
};

} // namespace tinyio::detail
```

`IO` is refactored to own a `FiberScheduler` by value plus the io_uring-specific members. Its public API is unchanged.

### 5.3 SimLoop

**File**: `include/tinyio/sim/sim_loop.hpp`

The `SimLoop` replaces the io_uring ring as the completion source for `SimIO`. `SimPipe` and `SimFile` call `SimLoop::post()` directly when a virtual operation finishes or should fail.

```cpp
namespace tinyio::sim {

struct SimCompletion {
    detail::fiber::Operation* op;
    int32_t                   result;  // positive = bytes, negative = -errno
};

class SimLoop {
public:
    /// Called by SimPipe / SimFile when a virtual I/O operation has a result.
    void post(detail::fiber::Operation* op, int32_t result) noexcept;

    /// Called by SimIO::tick(). Drains all pending completions, wakes fibers.
    /// Returns the number of completions processed.
    std::size_t drain(detail::FiberScheduler& sched) noexcept;

    bool has_pending() const noexcept { return !pending_.empty(); }

private:
    std::deque<SimCompletion> pending_;
};

} // namespace tinyio::sim
```

### 5.4 SimIO

**Files**: `include/tinyio/sim/sim_io.hpp`, `src/tinyio/sim/sim_io.cc`

`SimIO` mirrors `IO`'s public API exactly. It additionally exposes test-control methods.

```cpp
namespace tinyio::sim {

class SimIO {
public:
    explicit SimIO(const IoOptions& opts = {});

    // ── Public API identical to IO ──────────────────────────────────────
    template<class Fn> Result<FiberId> spawn(Fn&&) noexcept;
    template<class Fn> bool            post(Fn&&) noexcept;
    Result<void>   yield() noexcept;
    void           run_until_done() noexcept;
    void           run(const std::stop_token&) noexcept;
    bool           has_live_fibers() const noexcept;
    bool           is_owner_thread() const noexcept;

    // I/O operations (same signatures as IO)
    Result<uint32_t> read(const Fd&, std::span<std::byte>, off_t = -1) noexcept;
    Result<uint32_t> write(const Fd&, std::span<const std::byte>, off_t = -1) noexcept;
    Result<Fd>       accept(const Fd& server_fd, int flags = kDefaultAcceptFlags) noexcept;
    Result<void>     connect(const Fd&, const SocketAddress&) noexcept;
    Result<uint32_t> read(const Fd&, std::span<std::byte>, std::chrono::nanoseconds, off_t = -1) noexcept;
    Result<uint32_t> write(const Fd&, std::span<const std::byte>, std::chrono::nanoseconds, off_t = -1) noexcept;
    Result<void>     sleep_for_ns(std::chrono::nanoseconds) noexcept;
    // ... remaining ops

    // ── Test-only control API ────────────────────────────────────────────

    /// Advance simulated time by `duration`.
    /// Fires all timers whose deadline has passed and runs woken fibers.
    void advance_time(std::chrono::nanoseconds duration) noexcept;

    /// Create a connected pair of virtual sockets.
    /// Equivalent to socketpair() but entirely in-memory.
    std::pair<Fd, Fd> make_pipe() noexcept;

    /// Open a virtual file backed by an in-memory buffer.
    Fd open_sim_file(std::string_view name, int flags) noexcept;

    /// Access the underlying SimPipe for a given fd (for fault injection).
    SimPipe& get_pipe(Fd) noexcept;

    /// Access the underlying SimFile for a given fd (for fault injection).
    SimFile& get_file(Fd) noexcept;

    // Scheduler hooks needed by fiber_sync.hpp primitives
    detail::fiber::Fiber* running_fiber() const noexcept;
    void park_running() noexcept;
    void unpark(detail::fiber::Fiber*) noexcept;

private:
    absl::SimulatedClock                sim_clock_;
    detail::AbslSimClock                clock_wrapper_{sim_clock_};
    detail::FiberScheduler              scheduler_{&clock_wrapper_, opts_};
    SimLoop                             sim_loop_;
    std::unordered_map<int, SimPipe*>   pipes_;
    std::unordered_map<int, SimFile*>   files_;
    IoOptions                           opts_;

    void tick() noexcept;

    // Internal: submit a sim op and suspend the calling fiber.
    // prep is called with the Operation& to configure it before suspension.
    template<typename T, typename Prep>
    Result<T> sim_call(const char* op_name, Prep&&) noexcept;
};

} // namespace tinyio::sim
```

`SimIO::tick()` has no io_uring involvement:

```cpp
void SimIO::tick() noexcept {
    sim_loop_.drain(scheduler_);          // complete pending sim ops → push_ready
    scheduler_.process_expired_timers(    // fire deadline-exceeded timers
        [&](detail::fiber::Fiber* f) { sim_loop_.post(&f->operation, -ECANCELED); }
    );
    scheduler_.drain_remote();            // spawn cross-thread posted tasks
    scheduler_.resume_ready_fibers();     // run ready fibers (may post new sim ops)
}
```

### 5.5 SimPipe

**File**: `include/tinyio/sim/sim_pipe.hpp`

`SimPipe` models one direction of a virtual connection. A `make_pipe()` call creates two `SimPipe` ends wired to each other.

```cpp
namespace tinyio::sim {

class SimPipe {
public:
    // ── Fault injection ─────────────────────────────────────────────────

    /// Fail after exactly `n` bytes have been transferred, with `err`.
    SimPipe& fail_after_bytes(std::size_t n, int err = ECONNRESET) noexcept;

    /// Fail on the nth read or write operation (1-based).
    SimPipe& fail_on_nth_op(std::size_t n, int err) noexcept;

    /// Cap each individual read/write to at most `n` bytes (simulates short I/O).
    SimPipe& max_transfer_size(std::size_t n) noexcept;

    /// Drop the connection immediately.
    SimPipe& disconnect() noexcept;

    // ── Internal (called by SimIO) ───────────────────────────────────────
    void submit_read(detail::fiber::Operation* op, std::span<std::byte> buf) noexcept;
    void submit_write(detail::fiber::Operation* op, std::span<const std::byte> buf) noexcept;

private:
    SimLoop*          loop_      = nullptr;
    SimPipe*          peer_      = nullptr;
    std::deque<std::byte> buffer_;        // data written by peer, not yet read

    // Fault state
    std::optional<std::size_t> fail_after_bytes_;
    std::optional<std::size_t> fail_on_nth_op_;
    std::optional<std::size_t> max_transfer_;
    int                        fault_errno_ = ECONNRESET;
    std::size_t                bytes_transferred_ = 0;
    std::size_t                op_count_          = 0;
    bool                       disconnected_      = false;

    int32_t check_fault(std::size_t requested) noexcept;
};

} // namespace tinyio::sim
```

When `submit_write` is called, `SimPipe` applies fault checks, appends bytes to the peer's buffer, then calls `sim_loop_->post(op, bytes_written)`. If a pending read operation is waiting on the peer, it is also completed immediately.

### 5.6 SimFile

**File**: `include/tinyio/sim/sim_file.hpp`

`SimFile` backs virtual file fds. Data is stored in a `std::vector<std::byte>`.

```cpp
namespace tinyio::sim {

class SimFile {
public:
    // ── Fault injection ─────────────────────────────────────────────────

    /// Return `err` on the nth write (1-based).
    SimFile& fail_on_nth_write(std::size_t n, int err = EIO) noexcept;

    /// Return `err` on the nth read (1-based).
    SimFile& fail_on_nth_read(std::size_t n, int err = EIO) noexcept;

    /// Cap each write to at most `n` bytes (simulates partial writes).
    SimFile& max_write_size(std::size_t n) noexcept;

    /// Simulate a full disk: any write beyond `limit` bytes fails with ENOSPC.
    SimFile& capacity_limit(std::size_t limit) noexcept;

    // ── Inspection ──────────────────────────────────────────────────────

    /// Return the current contents of the virtual file.
    std::span<const std::byte> contents() const noexcept;

    // ── Internal ────────────────────────────────────────────────────────
    void submit_read(detail::fiber::Operation* op,
                     std::span<std::byte> buf, off_t offset) noexcept;
    void submit_write(detail::fiber::Operation* op,
                      std::span<const std::byte> buf, off_t offset) noexcept;

private:
    SimLoop*               loop_ = nullptr;
    std::vector<std::byte> data_;

    // Fault state
    std::optional<std::size_t> fail_on_nth_write_;
    std::optional<std::size_t> fail_on_nth_read_;
    std::optional<std::size_t> max_write_size_;
    std::optional<std::size_t> capacity_limit_;
    int                        write_fault_errno_ = EIO;
    int                        read_fault_errno_  = EIO;
    std::size_t                write_count_        = 0;
    std::size_t                read_count_         = 0;
};

} // namespace tinyio::sim
```

---

## 6. Fault Injection Model

Faults are injected at the transport level on `SimPipe` or `SimFile` objects, not at individual `SimIO` call sites. This is intentional: it means fault injection works correctly even when application code retries internally, buffers partial reads, or uses higher-level abstractions built on top of raw I/O ops.

**Fault precedence** (checked in order on each operation):

1. `disconnected_` / hard disconnect — returns `ECONNRESET` (pipe) or `EIO` (file) immediately.
2. `fail_on_nth_op_` / `fail_on_nth_write_` / `fail_on_nth_read_` — fires on the exact nth call.
3. `fail_after_bytes_` — fires when cumulative transferred bytes exceed the threshold.
4. `max_transfer_size_` / `max_write_size_` — silently caps the transfer size (not an error, causes short I/O).
5. `capacity_limit_` — `ENOSPC` when file size would exceed limit.

If no fault applies, the operation succeeds normally.

---

## 7. Time Control

All timer-dependent behavior in `SimIO` is driven by `advance_time()`:

```cpp
sim.advance_time(std::chrono::seconds(30));
```

This calls `sim_clock_.Advance(absl::Seconds(30))`, then fires all timer-wheel entries whose deadline has passed, then runs the woken fibers. The entire sequence is synchronous from the test's perspective.

**`sleep_for_ns()` in `SimIO`** arms a timer wheel entry and suspends the fiber — it never issues a kernel sleep or io_uring timeout SQE. The fiber only wakes when `advance_time()` pushes the clock past the deadline.

**Timed I/O ops** (`read(..., timeout)`, `write(..., timeout)`) behave identically: the timer is armed in the wheel; if `advance_time()` fires it before the I/O completes, the fiber wakes with `ETIMEDOUT`.

This enables tests like:

```cpp
// A 30-second idle timeout that resolves instantly:
sim.advance_time(std::chrono::seconds(31));
// The fiber waiting on read(..., 30s) is now ready with ETIMEDOUT.
sim.run_until_done();
```

---

## 8. File Layout

```
include/tinyio/
  detail/
    clock.hpp          ← NEW: Clock interface, RealClock, AbslSimClock
    scheduler.hpp      ← NEW: FiberScheduler (extracted from IO)
    fiber.hpp          ← unchanged
    timer_wheel.hpp    ← unchanged (monotonic_now_ns() retained as free fn)
    mpsc_queue.hpp     ← unchanged
  sim/
    sim_io.hpp         ← NEW: SimIO class
    sim_loop.hpp       ← NEW: SimLoop (in-process completion queue)
    sim_pipe.hpp       ← NEW: SimPipe (virtual socket with fault injection)
    sim_file.hpp       ← NEW: SimFile (virtual file with fault injection)
  runtime.hpp          ← modified: IO composes FiberScheduler
  fiber_sync.hpp       ← unchanged
  buffer_pool.hpp      ← unchanged

src/tinyio/
  scheduler.cc         ← NEW: FiberScheduler implementation (moved from runtime.cc)
  runtime.cc           ← modified: IO keeps io_uring logic, delegates to scheduler_
  sim/
    sim_io.cc          ← NEW
    sim_loop.cc        ← NEW
    sim_pipe.cc        ← NEW
    sim_file.cc        ← NEW
```

---

## 9. Impact on Existing Code

### `IO` class

The public API is **unchanged**. Internally:

- `IO` gains a `detail::FiberScheduler scheduler_` member (by value).
- `IO` gains a `detail::RealClock real_clock_` member; its address is passed to the scheduler.
- Methods that were direct members of `IO` and belong to the scheduler are delegated: `push_ready`, `pop_ready`, `remove_ready`, `resume_fiber`, `suspend_fiber`, `spawn_task`, `destroy_fiber`, `park_running`, `unpark`, `has_live_fibers`.
- The 3 `detail::monotonic_now_ns()` call sites in `runtime.cc` become `scheduler_.clock_->now_ns()`.
- `tick()` is unchanged in structure; only the delegation to `scheduler_.*` methods changes.

### `fiber_sync.hpp` (`Latch`, `Semaphore`, `Mutex`, `TaskGroup`, `ResultGroup`)

Unchanged. These call `io_.running_fiber()`, `io_.park_running()`, and `io_.unpark()` — all delegated methods that remain on `IO` and are also present on `SimIO`.

### `timer_wheel.hpp`

Unchanged. The timer wheel already accepts `now_ns` as a parameter.

### `buffer_pool.hpp`

Unchanged. Fixed buffers are io_uring-specific and remain on `IO` only; `SimIO` does not offer `write_fixed` / `read_fixed`.

### Tests

Existing tests that use `IO` directly are unaffected. New tests use `SimIO` instead and gain access to `advance_time()`, `make_pipe()`, `open_sim_file()`, and the fault injection API on `SimPipe` / `SimFile`.

---

## 10. Usage Examples

### Basic fiber test without io_uring

```cpp
#include <tinyio/sim/sim_io.hpp>

TEST(FiberTest, TaskGroupJoinsAllFibers) {
    tinyio::sim::SimIO sim;

    std::atomic<int> counter{0};
    sim.spawn([&](auto& io) noexcept {
        tinyio::TaskGroup group(io);
        for (int i = 0; i < 8; ++i) {
            (void)group.spawn([&](auto&) noexcept { ++counter; });
        }
        (void)group.wait();
        assert(counter == 8);
    });

    sim.run_until_done();
}
```

### Echo handler test with virtual socket

```cpp
TEST(EchoTest, HandlerRoundTrips) {
    tinyio::sim::SimIO sim;
    auto [client, server] = sim.make_pipe();

    sim.spawn([&](auto& io) noexcept {
        // server side: read and echo
        std::byte buf[64];
        auto n = io.read(server, buf).value();
        (void)io.write(server, std::span{buf, n});
    });

    sim.spawn([&](auto& io) noexcept {
        // client side: write then read back
        const auto msg = std::as_bytes(std::span{"hello"});
        (void)io.write(client, msg);
        std::byte reply[64];
        auto n = io.read(client, reply).value();
        assert(std::string_view{(char*)reply, n} == "hello");
    });

    sim.run_until_done();
}
```

### Timeout test — no real sleep

```cpp
TEST(TimeoutTest, ReadTimesOutCorrectly) {
    tinyio::sim::SimIO sim;
    auto [client, server] = sim.make_pipe();

    std::error_code ec;
    sim.spawn([&](auto& io) noexcept {
        std::byte buf[64];
        auto r = io.read(client, buf, std::chrono::seconds(5));
        if (!r) ec = r.error().error_code();
    });

    // Nobody writes to `server`, so the read will time out.
    // Advance time 5 seconds instantly — no real sleep.
    sim.advance_time(std::chrono::seconds(5));
    sim.run_until_done();

    EXPECT_EQ(ec, std::errc::timed_out);
}
```

### Fault injection — connection reset mid-stream

```cpp
TEST(FaultTest, ConnectionResetAfter1KB) {
    tinyio::sim::SimIO sim;
    auto [client, server] = sim.make_pipe();

    // Server will send 4KB, but connection resets after 1KB.
    sim.get_pipe(server).fail_after_bytes(1024, ECONNRESET);

    std::error_code ec;
    sim.spawn([&](auto& io) noexcept {
        std::byte buf[4096];
        std::memset(buf, 0xAB, sizeof(buf));
        auto r = io.write(server, buf);
        if (!r) ec = r.error().error_code();
    });

    sim.run_until_done();
    EXPECT_EQ(ec, std::errc::connection_reset);
}
```

### Fault injection — partial writes expose short-write handling

```cpp
TEST(FaultTest, WriterHandlesPartialWrites) {
    tinyio::sim::SimIO sim;
    auto [client, server] = sim.make_pipe();

    // Only 10 bytes written at a time regardless of request size.
    sim.get_pipe(server).max_transfer_size(10);

    sim.spawn([&](auto& io) noexcept {
        // A correct implementation loops until all bytes are sent.
        std::byte buf[100];
        std::size_t sent = 0;
        while (sent < sizeof(buf)) {
            auto n = io.write(server, std::span{buf + sent, sizeof(buf) - sent}).value();
            sent += n;
        }
        assert(sent == 100);
    });

    sim.run_until_done();
}
```

### Disk fault — ENOSPC mid-write

```cpp
TEST(FaultTest, WriterHandlesDiskFull) {
    tinyio::sim::SimIO sim;
    auto fd = sim.open_sim_file("test.dat", O_WRONLY | O_CREAT);

    // Disk fills up after 512 bytes.
    sim.get_file(fd).capacity_limit(512);

    std::error_code ec;
    sim.spawn([&](auto& io) noexcept {
        std::byte buf[1024];
        auto r = io.write(fd, buf);
        if (!r) ec = r.error().error_code();
    });

    sim.run_until_done();
    EXPECT_EQ(ec, std::errc::no_space_on_device);
}
```

---

## 11. Non-Goals

- **macOS / non-Linux production support.** `SimIO` is for testing. The production runtime remains Linux io_uring only.
- **Performance parity with `IO`.** `SimIO` is not benchmarked and does not need to be fast.
- **Full `IO` API surface on `SimIO`.** `write_fixed`, `read_fixed`, `register_file`, `fallocate`, `ftruncate` are io_uring-specific or tied to real file descriptors. They are not implemented in `SimIO`. Calling them returns `ENOSYS`.
- **Thread safety of `SimPipe` / `SimFile`.** Like `IO`, `SimIO` is single-threaded. All sim operations must occur on the owner thread.
- **Persistent SimFile state across `SimIO` instances.** Virtual files are owned by `SimIO` and destroyed with it.

---

## 12. Open Questions

1. **`SimIO` and `fiber_sync.hpp`**: `TaskGroup`, `Latch`, etc. take `IO&` today. Should they be templated on the runtime type, or should `SimIO` expose the same hook methods (`running_fiber`, `park_running`, `unpark`) so the primitives work without changes? The current plan is the latter — minimal change.

2. **Fd numbering in `SimIO`**: Real fds from the kernel start at 3+. `SimIO` needs a synthetic fd space for `make_pipe()` and `open_sim_file()`. Using negative integers or a high offset (e.g., `0x80000000+`) avoids collision with any accidentally-leaked real fds.

3. **`accept_loop` (multishot) in `SimIO`**: The production `IO::accept_loop` uses `IORING_ACCEPT_MULTISHOT`. `SimIO` can simulate this with a `SimListener` that repeatedly calls the handler as connections are pushed via a test API. Design TBD.

4. **absl dependency scope**: `absl::SimulatedClock` is only needed in `sim/`. The production headers (`runtime.hpp`, `fiber_sync.hpp`) must not pull in absl. The `AbslSimClock` wrapper lives in `sim/sim_io.hpp` only, not in `detail/clock.hpp`.

5. **CMake target**: `SimIO` should be a separate CMake target (`tinyio_sim`) that depends on `tinyio` but is not linked into production builds. Test targets link `tinyio_sim`; production targets link `tinyio` only.
