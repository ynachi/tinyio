# RFC-002: SimRing — In-Memory io_uring Emulation for tinyio

**Status**: Draft
**Author**: tinyio contributors
**Created**: 2026-06-22
**Updated**: 2026-06-27

---

## Table of Contents

1. [Abstract](#1-abstract)
2. [Motivation](#2-motivation)
3. [Design Decisions Considered](#3-design-decisions-considered)
   - 3.1 [Simulation Depth](#31-simulation-depth)
   - 3.2 [Architecture Options](#32-architecture-options)
   - 3.3 [Cross-Platform vs Sim-Only](#33-cross-platform-vs-sim-only)
4. [Chosen Design: io_uring Emulation](#4-chosen-design-io_uring-emulation)
5. [Component Breakdown](#5-component-breakdown)
   - 5.1 [RingBackend Interface](#51-ringbackend-interface)
   - 5.2 [SimRing](#52-simring)
   - 5.3 [SimPipe & SimFile](#53-simpipe--simfile)
   - 5.4 [Clock Abstraction](#54-clock-abstraction)
6. [Fault Injection Model](#6-fault-injection-model)
7. [Time Control](#7-time-control)
8. [File Layout](#8-file-layout)
9. [Impact on Existing Code](#9-impact-on-existing-code)
10. [Usage Examples](#10-usage-examples)
11. [Non-Goals](#11-non-goals)
12. [Open Questions](#12-open-questions)

---

## 1. Abstract

This RFC introduces `SimRing`, a user-space emulation of the Linux io_uring interface for tinyio. Rather than creating a separate `SimIO` class and fracturing the public API, this design allows the standard `tinyio::IO` class to operate seamlessly against a mock kernel ring. 

It enables deterministic unit testing of fiber logic, protocol handlers, timeout behavior, and failure paths — without requiring a real io_uring, real network interfaces, or a real disk. Crucially, application code remains completely unchanged and does not need to be templated.

---

## 2. Motivation

tinyio targets Linux io_uring for production. This creates friction for:

- **Unit testing**: Tests require a real io_uring ring, real fds, and real network. Setup is heavy, tests are slow, and they often flake due to environment dependencies.
- **Fault injection**: Inducing `ECONNRESET`, `EIO`, or partial writes against real kernel interfaces requires privileged tools (iptables, fault injection modules) or fragile wrapper scripts.
- **Timeout testing**: Verifying that a fiber correctly handles a 30-second idle timeout requires waiting 30 seconds, or awkwardly bypassing the logic entirely.
- **CI portability**: Environments without robust io_uring support (restricted containers, older kernels, macOS development machines) cannot run any tests today.

The goal is a testing ecosystem that:

1. Compiles and runs on any OS (no io_uring kernel required in test builds).
2. Provides virtual files and virtual sockets backed by in-memory buffers.
3. Allows injecting arbitrary errors at the transport level.
4. Allows advancing simulated time instantly to trigger timeouts.
5. **Requires zero changes to application fiber code** — handlers written against `IO` work identically and take an `IO&`.

---

## 3. Design Decisions Considered

### 3.1 Simulation Depth

Three levels of simulation were evaluated:

| Level | Description                         | Fault injection | Time control | API Impact                           |
|-------|-------------------------------------|-----------------|--------------|--------------------------------------|
| 1     | epoll + blocking POSIX shim         | Limited         | No           | Minimal                              |
| 2     | High-level simulated `SimIO` class  | White-box only  | Partial      | High (requires templating user code) |
| 3     | Emulate io_uring ring in user-space | Transport-level | Full         | Zero                                 |

**Level 3 was chosen.** It enables perfect transport-level fault injection and instant time control, while keeping the public `IO` API and its internal SQE/CQE processing logic completely unmodified.

### 3.2 Architecture Options

Three structural options were considered for how simulation integrates with production code:

**Option A — Shared virtual base class.**
Extract an `IoBase` abstract class. Both `IO` and `SimIO` inherit from it. User code takes `IoBase&`.
*Rejected:* Abstracting the `syscall<T>` SQE preparation step requires a fat op-descriptor union or per-backend methods, introducing virtual dispatch on every I/O operation in production.

**Option B — Extracted `FiberScheduler`, composed into both `IO` and `SimIO`.**
The original draft of this RFC proposed extracting the fiber management into a standalone `FiberScheduler`, and having distinct `IO` and `SimIO` classes that compose it.
*Rejected:* This meant `SimIO` and `IO` were different types. Application code like `void handle_client(IO& io, Fd fd)` would have to be templated to `void handle_client(auto& io, Fd fd)` just to enable testing. This is a severe usability degradation.

**Option C — `io_uring` Emulation (SimRing).**
Instead of touching `IO`'s public API, abstract the *kernel boundary*. `IO` formats standard `io_uring_sqe` structs and consumes standard `io_uring_cqe` structs. A backend driver either submits these to the kernel (production) or processes them in user-space against virtual fds (simulation).
*Chosen.* See §4.

### 3.3 Cross-Platform vs Sim-Only

macOS and other non-Linux platforms were evaluated as targets. 
**Sim-only was chosen.** tinyio is Linux-first. A full kqueue backend would add massive maintenance overhead. `SimRing` solves the developer experience problem by allowing dev/test to run anywhere, without compromising the Linux-specific nature of the production runtime.

---

## 4. Chosen Design: io_uring Emulation

```text
                  Application Code
                         │
                 tinyio::IO (unchanged)
            (Formats SQEs, processes CQEs)
                         │
            ┌────────────┴─────────────┐
            │        IO::tick()        │ (kernel boundary)
            │      if (simulated_)     │
            └──────┬─────────────┬─────┘
                   │             │
        (false)    │             │ (true)
                   │             │
   ::io_uring_enter              SimRing::submit_and_wait
 (Linux System Call)              (User-space emulation)
                                       │
                                ┌──────┴──────┐
                             SimPipe       SimFile
                          (in-memory      (in-memory
                           socket pair)    byte buffer)
```

1. `IO` continues to manage its own SQ and CQ ring memory buffers, but these can now be backed by standard heap memory instead of kernel `mmap` if in simulation mode.
2. The `io_uring_enter` system call is replaced by a direct boolean branch (`if (sim_ring_)`) in the event loop `tick()`. This eliminates the need for virtual dispatch (`vtable`) entirely. Since the pointer is stable during the object's lifetime, CPU branch prediction is 100% accurate, resulting in exactly zero pipeline stalls.
3. In tests, the test instantiates a `SimRing` and passes its pointer to the `IO` constructor via `IoOptions{.sim_ring = &sim}`. The `IO` class wires itself up to the provided simulation ring.
4. User code operates against `IO&`. Time control and fault injection are accessed by interacting with the explicitly owned `SimRing` instance.

---

## 5. Component Breakdown

### 5.1 Branching in `IO::tick()`

To achieve zero dynamic dispatch overhead without resorting to templates, `IO::tick()` directly evaluates the `sim_ring_` pointer:

```cpp
void IO::tick() noexcept {
    // ... format SQEs ...
    
    int submitted;
    if (sim_ring_) {
        // Test mode: call the SimRing emulator
        submitted = sim_ring_->submit_and_wait(sq_.sqe_tail - sq_.sqe_head, 1, 0);
    } else {
        // Production mode: call the Linux kernel directly
        submitted = ::io_uring_enter(ring_fd_, sq_.sqe_tail - sq_.sqe_head, 1, 0, nullptr);
    }
    
    // ... process CQEs ...
}
```

### 5.2 SimRing

**File**: `include/tinyio/sim/sim_ring.hpp`

`SimRing` implements `RingBackend`. When `submit_and_wait` is called, it iterates through the available `io_uring_sqe` entries, decodes the opcodes (`IORING_OP_READ`, `IORING_OP_WRITE`, `IORING_OP_TIMEOUT`, etc.), and executes them against its virtual file descriptor table. It then populates `io_uring_cqe` entries.

```cpp
namespace tinyio::sim {

class SimRing {
public:
    SimRing() = default;

    // Called by IO constructor to wire up the ring pointers
    void attach(io_uring_sq* sq, io_uring_cq* cq) noexcept;

    int submit_and_wait(unsigned to_submit, unsigned min_complete, unsigned flags) noexcept;

    // ── Test-only control API ────────────────────────────────────────────

    /// Advance simulated time by `duration`.
    /// Fires IORING_OP_TIMEOUT SQEs whose deadline has passed.
    void advance_time(std::chrono::nanoseconds duration) noexcept;

    /// Create a connected pair of virtual sockets.
    std::pair<Fd, Fd> make_pipe() noexcept;

    /// Open a virtual file backed by an in-memory buffer.
    Fd open_sim_file(std::string_view name, int flags) noexcept;

    /// Access the underlying SimPipe/SimFile (for fault injection).
    SimPipe& get_pipe(Fd fd) noexcept;
    SimFile& get_file(Fd fd) noexcept;

private:
    io_uring_sq* sq_;
    io_uring_cq* cq_;
    absl::SimulatedClock clock_;
    std::unordered_map<int, std::unique_ptr<SimPipe>> pipes_;
    std::unordered_map<int, std::unique_ptr<SimFile>> files_;
    
    // Internal dispatch
    void process_sqe(const io_uring_sqe* sqe) noexcept;
    void post_cqe(uint64_t user_data, int32_t res, uint32_t flags) noexcept;
};

} // namespace tinyio::sim
```

### 5.3 SimPipe & SimFile

These operate as the simulated endpoints for the io_uring read/write operations. They contain memory buffers and the state required for fault injection. When `SimRing` processes an `IORING_OP_WRITE` against a simulated pipe fd, it routes the payload to the corresponding `SimPipe`.

```cpp
namespace tinyio::sim {

class SimPipe {
public:
    // ── Fault injection ─────────────────────────────────────────────────
    SimPipe& fail_after_bytes(std::size_t n, int err = ECONNRESET) noexcept;
    SimPipe& fail_on_nth_op(std::size_t n, int err) noexcept;
    SimPipe& max_transfer_size(std::size_t n) noexcept;
    SimPipe& disconnect() noexcept;

    // Internal execute called by SimRing
    int32_t execute_read(std::span<std::byte> buf) noexcept;
    int32_t execute_write(std::span<const std::byte> buf) noexcept;
};

class SimFile {
    // Similar fault APIs (fail_on_nth_write, capacity_limit, etc.)
    // Internal execute called by SimRing
};

}
```

### 5.4 Clock Abstraction

**File**: `include/tinyio/detail/clock.hpp`

Instead of coupling the timer wheel directly to `CLOCK_MONOTONIC`, we allow `IO` to query its active backend. In production, it returns hardware time. In simulation, it returns `absl::SimulatedClock` time.

---

## 6. Fault Injection Model

Faults are injected at the transport level on `SimPipe` or `SimFile` objects. This ensures fault injection works correctly even when application code retries internally or uses high-level `IO` primitives.

**Fault precedence** (checked in order on each operation):

1. Hard disconnect — returns `-ECONNRESET` (pipe) or `-EIO` (file) immediately via CQE.
2. `fail_on_nth_op_` — fires on the exact nth call.
3. `fail_after_bytes_` — fires when cumulative transferred bytes exceed threshold.
4. `max_transfer_size_` — silently caps the transfer size (returns a short I/O result).
5. `capacity_limit_` — `-ENOSPC` when file size would exceed limit.

---

## 7. Time Control

All timer-dependent behavior relies on `IORING_OP_TIMEOUT`. 
When `IO::sleep_for_ns()` is called, an SQE is submitted. 

In a test, time is advanced synchronously:

```cpp
// Advance simulated time by 30 seconds instantly.
sim_ring.advance_time(std::chrono::seconds(30));
```

Internally, `advance_time` updates the virtual clock and immediately posts CQEs to the completion ring for any `IORING_OP_TIMEOUT` SQEs that have expired, waking the waiting fibers on the next `IO::tick()`.

---

## 8. File Layout

```text
include/tinyio/
  detail/
    clock.hpp          ← NEW: Clock interface for timer wheel
    fiber.hpp          ← unchanged
    timer_wheel.hpp    ← modified to take Clock*
  sim/
    sim_ring.hpp       ← NEW: User-space io_uring emulator
    sim_pipe.hpp       ← NEW: SimPipe (virtual socket with fault injection)
    sim_file.hpp       ← NEW: SimFile (virtual file with fault injection)
  io.hpp               ← modified: Adds IoOptions{.sim_ring}

src/tinyio/
  io.cc                ← modified: tick() branches on sim_ring_ pointer
  sim/
    sim_ring.cc        ← NEW
    sim_pipe.cc        ← NEW
    sim_file.cc        ← NEW
```

---

## 9. Impact on Existing Code

### `IO` class
- `IO` gains a `sim_ring` pointer in `IoOptions` to construct in simulated mode.
- Internally, `IO` directly branches to call `sim_ring_->submit_and_wait()` if the pointer is non-null, eliminating any virtual call overhead.
- No extracted `FiberScheduler` is required. The `IO` class maintains its exact internal design, ensuring tests exercise the exact production codepath (SQE formatting, CQE consumption, fiber waking).

### `fiber_sync.hpp`
Unchanged. `TaskGroup`, `Latch`, `Mutex`, etc., continue to take `IO&` and work seamlessly.

### User Handlers
Zero changes. User handlers take `IO&`.

---

## 10. Usage Examples

### Basic Handler Test

```cpp
#include <tinyio/sim/sim_ring.hpp>

TEST(EchoTest, HandlerRoundTrips) {
    // 1. Test owns the simulation controller explicitly
    tinyio::sim::SimRing sim;
    
    // 2. IO wires itself up to the provided simulation ring
    tinyio::IO io{tinyio::IoOptions{.sim_ring = &sim}};
    
    auto [client, server] = sim.make_pipe();

    io.spawn([&](auto& io_ref) noexcept {
        // server side: read and echo
        std::byte buf[64];
        auto n = io_ref.read(server, buf).value();
        (void)io_ref.write(server, std::span{buf, n});
    });

    io.spawn([&](auto& io_ref) noexcept {
        // client side: write then read back
        const auto msg = std::as_bytes(std::span{"hello"});
        (void)io_ref.write(client, msg);
        std::byte reply[64];
        auto n = io_ref.read(client, reply).value();
        assert(std::string_view{(char*)reply, n} == "hello");
    });

    io.run_until_done();
}
```

### Timeout Test — No Real Sleep

```cpp
TEST(TimeoutTest, ReadTimesOutCorrectly) {
    tinyio::sim::SimRing sim;
    tinyio::IO io{tinyio::IoOptions{.sim_ring = &sim}};
    
    auto [client, server] = sim.make_pipe();

    std::error_code ec;
    io.spawn([&](auto& io_ref) noexcept {
        std::byte buf[64];
        auto r = io_ref.read(client, buf, std::chrono::seconds(5));
        if (!r) ec = r.error().error_code();
    });

    // Nobody writes to `server`, so the read will time out.
    // Advance time 5 seconds instantly — no real sleep.
    sim.advance_time(std::chrono::seconds(5));
    io.run_until_done();

    EXPECT_EQ(ec, std::errc::timed_out);
}
```

### Fault Injection

```cpp
TEST(FaultTest, ConnectionResetAfter1KB) {
    tinyio::sim::SimRing sim;
    tinyio::IO io{tinyio::IoOptions{.sim_ring = &sim}};
    
    auto [client, server] = sim.make_pipe();

    // Server will send 4KB, but connection resets after 1KB.
    sim.get_pipe(server).fail_after_bytes(1024, ECONNRESET);

    std::error_code ec;
    io.spawn([&](auto& io_ref) noexcept {
        std::byte buf[4096];
        std::memset(buf, 0xAB, sizeof(buf));
        auto r = io_ref.write(server, buf);
        if (!r) ec = r.error().error_code();
    });

    io.run_until_done();
    EXPECT_EQ(ec, std::errc::connection_reset);
}
```

---

## 11. Non-Goals

- **Performance parity with `io_uring_enter`.** `SimRing` is not benchmarked and does not need to be fast.
- **Full `io_uring` support.** `SimRing` only needs to emulate the specific subset of `io_uring` opcodes that tinyio actually uses (`IORING_OP_READ`, `IORING_OP_WRITE`, `IORING_OP_TIMEOUT`, `IORING_OP_POLL_ADD`, etc.). Unimplemented opcodes will panic or return `-ENOSYS`.
- **macOS / non-Linux production support.** `SimRing` is strictly for dev/test environments.

---

## 12. Open Questions

1. **Memory Allocation for Simulation Rings:** In production, `io_uring_setup` gives us kernel memory via `mmap`. `SimRing` will need to just `calloc` the same struct layouts and hand the pointers to `IO`.
2. **`IORING_OP_POLL_ADD` emulation:** How fully do we need to emulate epoll-like readiness semantics in `SimRing`? Likely, we can short-circuit this by checking if the virtual fd has buffered data.
3. **CMake target:** `SimRing` should likely live in a separate library (`tinyio_sim`) so `absl` does not leak into the production binary.
