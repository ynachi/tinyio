# RFC: Deterministic Simulation for tinyio via Event-Loop Interception

- **Status**: Proposed
- **Date**: 2026-10-25
- **Target Area**: `src/tinyio/runtime/`, `src/tinyio/sim/`

---

## 1. Executive Summary

This RFC specifies a **Deterministic Simulation Testing (DST)** mode for `tinyio` built by **intercepting time, scheduling, CQE delivery order, and fault injection directly inside `IO`'s event loop while keeping real `io_uring` system calls, real registered `FixedBuffer`s, and real file descriptors**.

Rather than re-implementing a fake in-memory filesystem (`SimDisk`), a fake TCP/IP stack (`SimNetwork`), or a separate discrete-event scheduler (`SimWorld`), `tinyio` runs its **actual** `io_uring` submission/completion pipeline and `TimerWheel`, while a lightweight `SimController` attached to `IO` takes ownership of:

1. **The Clock (`IO::NowNs()`)**: Driven by a virtual monotonic clock `sim_now_ns_` that jumps forward instantly when no fibers are runnable.
2. **Completion Ordering (`DrainCompletions()`)**: Reaped `io_uring` CQEs are placed into a **Virtual-Time Staging Min-Heap** keyed by a PRNG-assigned virtual completion timestamp `complete_at_ns`, decoupling physical NVMe/OS jitter from the order and timestamp at which fibers wake up.
3. **Ready-Queue Dispatch (`ResumeReadyFibers()`)**: Concurrent ready fibers are popped in a seeded pseudo-random order (chaos scheduling) instead of strict FIFO.
4. **Pre-Submission Fault & Crash Injection (`Syscall()`, `WriteFixed()`, `Fsync()`)**: Torn writes, short reads, `EIO`/`ENOSPC` errors, and un-`Fsync`ed power-cut rollbacks are applied at the `Syscall()` boundary on real files.

---

## 2. Core Questions & Architectural Q&A

### Q0: If `IO` is dual-mode, why would we need `SimDisk`, `SimNetwork`, and `SimWorld`? Isn't simulating time and intercepting in the event loop enough?

**Answer: Simulating time and intercepting in the event loop is enough — and is strictly superior for `tinyio`.**

Full user-space mocks (`SimDisk`, `SimNetwork`, `SimWorld`) only exist in frameworks that either cannot execute real kernel I/O or want to model a 50-node WAN in memory. Building them for `tinyio` would have two major drawbacks:
1. **You end up testing the mock instead of `tinyio`:** A fake `SimDisk` bypasses `io_uring_get_sqe`, `io_uring_register_buffers`, `FixedBuffer` `buf_index` arithmetic, `Operation` user_data pointer tagging, and CQ ring advancement.
2. **10× more code to maintain:** Every POSIX edge case (`O_DIRECT` alignment, `fallocate`, `ftruncate`, `preadv2`, `pwritev2`, `rename`, socket half-close) would have to be emulated by hand.

By intercepting inside `IO`'s event loop (`Tick`, `ResumeReadyFibers`, `Syscall`, `SubmitOrWait`, `DrainCompletions`, `ProcessExpiredTimers`), **100% of `tinyio`'s real `io_uring` and `FixedBufferPool` machinery runs on every test**, while the 4 sources of non-determinism (wall-clock time, hardware completion order, ready-queue tie-breaking, and hardware faults) are completely controlled by a single 64-bit PRNG seed.

---

### Q1: Do we execute real `io_uring` system calls or not?

**Answer: Yes.**

Every `Open`, `Read`, `Write`, `ReadFixed`, `WriteFixed`, `Readv`, `Writev`, `Fsync`, `Fallocate`, `Ftruncate`, `Rename`, `Remove`, and `Close` still allocates a real `io_uring_sqe`, attaches the fiber's `&current_fiber_->operation` via `io_uring_sqe_set_data64`, submits it to the Linux kernel via `io_uring_enter`, and reaps the resulting `io_uring_cqe`.

The only operations that bypass or modify the SQE before submission are:
- **`SleepForNs(duration)`**: Uses the virtual clock `sim_now_ns_` and `TimerWheel` (or staging heap) directly in userspace so a `30 s` sleep completes in `0 ns` of wall-clock time without asking the kernel to sleep.
- **Pre-submission fault injection**:
  - If the seeded PRNG decides an operation fails immediately with `-EIO` or `-ENOSPC`, `Syscall()` returns that error without submitting an SQE.
  - If the seeded PRNG decides a write is a **torn write** (e.g., during a simulated power cut), `Syscall()` clamps the SQE length (e.g., from `4096` bytes to `512` bytes) before submitting the real SQE to `io_uring`.

---

### Q2: If we execute real `io_uring` system calls on real hardware, how is it 100% deterministic?

**Answer: By staging kernel CQEs in a Virtual-Time Min-Heap before waking any fiber.**

#### Why raw `io_uring` is non-deterministic in production mode
Suppose Fiber A submits `ReadFixed(fd, slot_0, offset=0)` and Fiber B submits `ReadFixed(fd, slot_1, offset=4096)` during Phase 1 (`ResumeReadyFibers()`).
In production mode:
1. `SubmitOrWait()` calls `io_uring_submit_and_wait_timeout(&ring_, &cqe, /*wait_nr=*/1, ...)`.
2. That syscall returns as soon as **at least 1** CQE is posted by the NVMe interrupt handler.
3. On Run 1, the NVMe controller might finish A first (or finish only A before the syscall returns); on Run 2, a background kernel softirq or NVMe queue arbitration might finish B first, or finish both A and B before the syscall returns.
4. Because `DrainCompletions()` pushes fibers into `ready_queue_` in CQE arrival order, that physical timing jitter changes which fiber runs in the next tick.

#### How the CQE Staging Min-Heap eliminates hardware non-determinism
In simulation mode, we decouple **when the kernel executes the I/O** from **when the fiber observes the completion**:

```
Fiber A: ReadFixed() ──► assigned complete_at_ns = t0 + 55 µs (seq=1) ──┐
                                                                        ├──► io_uring_submit_and_wait(2)
Fiber B: ReadFixed() ──► assigned complete_at_ns = t0 + 20 µs (seq=2) ──┘          │
                                                                                   ▼
                                                            Both real reads finish in ~12 µs wall time
                                                                                   │
                                                                                   ▼
                                                            ┌─────────────────────────────────────────┐
                                                            │        CQE Staging Min-Heap             │
                                                            │  1. (t = 20 µs, seq = 2, Fiber B, 4096) │
                                                            │  2. (t = 55 µs, seq = 1, Fiber A, 4096) │
                                                            └──────────────────┬──────────────────────┘
                                                                               │
                                                    Pop earliest: sim_now_ns_ = 20 µs, wake Fiber B first!
```

1. **Deterministic Virtual Timestamp Assignment (at `Syscall()` time):**
   When a fiber prepares an SQE in `Syscall()`, `sim_` draws a virtual duration $\Delta t$ from the seeded PRNG (according to a configurable latency profile) and stamps the operation:
   ```cpp
   op.sim_complete_at_ns = sim_->NowNs() + sim_->SampleLatencyNs(op_kind);
   op.sim_seq = sim_->NextSeq();
   ```
2. **Barrier Harvest of Self-Completing Kernel Ops (at `SubmitOrWait()` time):**
   Instead of calling `io_uring_submit_and_wait_timeout(..., wait_nr = 1)` (which allows the kernel to return a non-deterministic subset of in-flight ops), simulation mode waits for **all** submitted self-completing SQEs in the current batch to complete in the kernel:
   ```cpp
   // Wait until every submitted file/disk SQE in this batch has produced a CQE:
   while (kernel_inflight_ > 0) {
       (void)io_uring_submit_and_wait(&ring_, static_cast<unsigned>(kernel_inflight_));
       HarvestKernelCqesToStagingHeap();
   }
   ```
   Because local disk/file operations (`Open`, `Read`, `Write`, `ReadFixed`, `WriteFixed`, `Fsync`, `Fallocate`, `Ftruncate`, `Close`) never depend on a future action by another fiber, `io_uring_submit_and_wait(&ring_, kernel_inflight_)` always completes in microseconds without deadlocking.
3. **Virtual-Time Release (in `DrainCompletions()`):**
   Once the real kernel CQEs are sitting in `staged_cqes_` (a min-heap ordered by `(sim_complete_at_ns, sim_seq)`):
   - The event loop compares the top of `staged_cqes_` (`next_cqe_ns`) against the next `TimerWheel` expiration (`next_timer_ns`).
   - It advances `sim_now_ns_ = std::min(next_cqe_ns, next_timer_ns)`.
   - Every staged CQE with `sim_complete_at_ns <= sim_now_ns_` is popped in exact `(sim_complete_at_ns, sim_seq)` order, writes `op->cqe_res`, disarms `timer_wheel_`, and calls `PushReady(op->fiber)`.

#### Consequence: Dynamic Causality Is Preserved
Suppose in Tick 1:
- Fiber A submits `Read` $\rightarrow$ stamped for `t = 55 µs`.
- Fiber B submits `Read` $\rightarrow$ stamped for `t = 20 µs`.
- Both real reads execute in the kernel in Tick 1 and enter `staged_cqes_`.
- The loop advances `sim_now_ns_` to `20 µs` and wakes **only Fiber B** (`t = 55 µs` stays in `staged_cqes_`).
- In Tick 2 (`sim_now_ns_ = 20 µs`), Fiber B resumes and submits a `Write` $\rightarrow$ stamped for `t = 20 µs + 25 µs = 45 µs`.
- That `Write` executes in the kernel in Tick 2 and enters `staged_cqes_` at `t = 45 µs`.
- The loop advances `sim_now_ns_` to `45 µs` and wakes **Fiber B again** — *before* Fiber A (`t = 55 µs`) ever wakes up!

Even though the real kernel executed Fiber A's read during Tick 1, Fiber A cannot observe its completion until virtual time reaches `55 µs`, and any writes Fiber B performs at `t = 20 µs` or `t = 45 µs` happen strictly earlier in virtual time.

*(Wait: what if Fiber B at `t = 20 µs` overwrites the exact same file block that Fiber A was reading at `t = 0..55 µs`? Because Fiber A's read was already executed by the kernel at `t = 0`, Fiber A read the old bytes—which is a valid concurrent read/write outcome! If you even want to randomize whether a concurrent overlapping read/write sees the old or new bytes, you can hold the SQE submission itself in the staging heap until `sim_execute_at_ns` before submitting it to `io_uring`!)*

Wait — read that parenthetical again! That is an even deeper insight:
- **Option A (Stage at CQE):** Submit SQEs immediately, stage CQEs in the min-heap until `sim_complete_at_ns`.
- **Option B (Stage SQE execution at `sim_complete_at_ns`):** Hold the prepared SQE parameters in the min-heap until `sim_now_ns_` reaches `sim_complete_at_ns`, and at that exact virtual timestamp submit the SQE to `io_uring` and reap it immediately!
Look at what **Option B (or staging SQE submission at virtual completion time)** gives you:
1. If Fiber A submits a slow `Read(block 0)` that completes at `t = 55 µs`, and Fiber B submits a fast `Write(block 0)` at `t = 20 µs` that completes at `t = 45 µs`, executing the kernel SQE at `sim_complete_at_ns` means Fiber B's write hits the file at `t = 45 µs` and Fiber A's read hits the file at `t = 55 µs`!
2. Even better: what about **sockets** (`Accept`, `Connect`, `Read` on a socket)?
   If Fiber 1 calls `Read(socket_fd)` at `t = 0`, and Fiber 2 calls `Write(peer_fd)` at `t = 30 µs`:
   With non-blocking `io_uring_submit_and_get_events` (`wait_nr = 0`) for sockets and barrier `io_uring_submit_and_wait` for self-completing file ops, both file ops and loopback socket ops work seamlessly!

---

### Q3: Can we fake kernel timing (latency distributions, tail stalls, and timeout races)?

**Answer: Yes, with 100% precision and zero real-time waiting.**

Because both the CQE Staging Min-Heap and `TimerWheel` read `IO::NowNs()` (`sim_now_ns_`), the simulator has total authority over perceived kernel timing:

#### 1. Arbitrary Latency Distributions & Tail Spikes
`SimConfig` can configure per-opcode latency distributions driven by `SimRng`:
- **Normal NVMe read/write:** Uniform or log-normal in `[10 µs, 80 µs]`.
- **Simulated GC / tail stall:** With probability `p = 0.01`, add a `50 ms` latency spike to a single `ReadFixed` or `Fsync`.
- Because `sim_now_ns_` jumps directly from event to event, a simulated `50 ms` NVMe stall or a `60 s` connection timeout executes in **nanoseconds of wall-clock time**.

#### 2. Deterministic Timeout vs. Completion Races (Zero Kernel Cancel Races)
In real life, testing what happens when an I/O operation times out (`ETIMEDOUT`) vs. completes just in time is notoriously flaky because `IORING_OP_ASYNC_CANCEL` races against the hardware interrupt in the kernel.

In simulation mode, timeout vs. completion is a **pure integer comparison** between two virtual timestamps:
1. Suppose at `sim_now_ns_ = 0`, a fiber calls:
   ```cpp
   auto res = io.ReadFixed(fd, buf, 4096, offset, 0, /*timeout=*/std::chrono::milliseconds(10));
   ```
2. `Syscall()` arms the `TimerWheel` with `op.deadline_ns = 10'000'000` (`10 ms`).
3. `SimController` draws a simulated kernel latency for this read from `SimRng`:
   - **Case A (`sim_complete_at_ns = 9'500'000`):** When the event loop advances `sim_now_ns_`, `9.5 ms < 10 ms`. The staged CQE is popped at `sim_now_ns_ = 9.5 ms`, disarms the `TimerWheel`, and the fiber deterministically resumes with `4096` bytes read.
   - **Case B (`sim_complete_at_ns = 10'500'000`):** The `TimerWheel` slot expires first (`10 ms < 10.5 ms`). At `sim_now_ns_ = 10 ms`, `ProcessExpiredTimers()` marks `op.timed_out = true`, intercepts the staged CQE in `staged_cqes_` (overwriting its `cqe_res = -ECANCELED` without even needing a racy kernel cancel!), and the fiber deterministically resumes with `ETIMEDOUT`!

---

## 3. Simulating Power Cuts, Torn Writes, and Un-`Fsync`ed Data Loss on Real Files

A key requirement for testing `tinyio::wal` is verifying crash recovery after a sudden power loss. How do we simulate a power cut on a **real Linux file** (`tmpfs` or `ext4`) without writing a fake in-memory `SimDisk`?

When a program writes to a real Linux file via `io_uring` (`WRITE` / `WRITE_FIXED`), the bytes land in the file immediately—so if we simply close the file descriptor without calling `Fsync()`, a reader opening that file on Linux would still see the un-`Fsync`ed bytes.

To model volatile drive caches and power cuts on a real file, `SimController` tracks a lightweight **Dirty Write Log** per open file descriptor between `Fsync()` calls:

```
┌────────────────────────────────────────────────────────────────────────────┐
│                Real Linux File + Dirty Write Tracker (Sim)                 │
├────────────────────────────────────────────────────────────────────────────┤
│ 1. On Write / WriteFixed(fd, buf, len, offset):                            │
│    - Save pre-write contents of [offset, offset + len) into DirtyWriteLog  │
│      (or record the dirty range + new bytes)                               │
│    - Submit the real WRITE / WRITE_FIXED SQE to io_uring                   │
│                                                                            │
│ 2. On Fsync(fd, datasync):                                                 │
│    - Submit the real FSYNC SQE to io_uring                                 │
│    - Clear DirtyWriteLog[fd] (all prior writes are now durable!)           │
│                                                                            │
│ 3. On SimController::SimulatePowerCut(fd):                                 │
│    - For each dirty write in DirtyWriteLog[fd] since the last Fsync():     │
│      * Roll SimRng:                                                        │
│        - Keep whole write (flushed by drive controller before power died)  │
│        - Revert to pre-write bytes (lost in volatile drive DRAM cache)     │
│        - Tear on 512B/4KiB boundary (first k sectors new, rest old/garbage)│
└────────────────────────────────────────────────────────────────────────────┘
```

Look at what this achieves:
- **During normal operation**, every `WriteFixed` and `ReadFixed` goes straight through `io_uring` on the real file using real registered `FixedBuffer`s.
- **When `Fsync()` completes**, the dirty log for `fd` is cleared (`O(1)` vector clear).
- **When `SimulatePowerCut()` is called**, the simulator uses `pwrite` on the real file to revert or tear a PRNG-selected subset of the writes that occurred *after* the last `Fsync()`, then abandons the old `IO` instance.
- A new `IO` instance then runs `wal::Reader::Recover()` on that **exact real file** through real `io_uring`!

---

## 4. Detailed Component Design

### 4.1. `SimConfig` and `SimRng` (`src/tinyio/sim/sim_controller.hpp`)

```cpp
namespace tinyio::sim {

/// Deterministic SplitMix64 + Xoshiro256++ PRNG.
class SimRng {
public:
    explicit constexpr SimRng(uint64_t seed) noexcept;

    [[nodiscard]] uint64_t NextU64() noexcept;
    [[nodiscard]] uint64_t Uniform(uint64_t min_inc, uint64_t max_inc) noexcept;
    [[nodiscard]] size_t Bounded(size_t bound) noexcept;
    [[nodiscard]] bool Bernoulli(double probability) noexcept;

private:
    uint64_t state_[4];
};

struct SimConfig {
    uint64_t seed = 1;

    /// Randomize ready_queue_ pop order in ResumeReadyFibers().
    bool chaos_scheduler = true;

    /// Virtual latency range (ns) assigned to normal I/O operations.
    uint64_t min_io_latency_ns = 10'000;      // 10 µs
    uint64_t max_io_latency_ns = 80'000;      // 80 µs

    /// Optional tail latency spike injection.
    double tail_spike_prob = 0.0;
    uint64_t tail_spike_latency_ns = 50'000'000; // 50 ms

    /// Probability of injecting a transient/permanent I/O error (EIO / ENOSPC).
    double io_error_prob = 0.0;
    int injected_errno = EIO;

    /// Probability of clamping a Write/WriteFixed to a partial sector on PowerCut.
    double torn_write_prob = 0.0;
};

}  // namespace tinyio::sim
```

---

### 4.2. Staged CQE Min-Heap Inside `SimController`

```cpp
struct StagedCqe {
    uint64_t complete_at_ns;
    uint64_t seq;
    uint64_t user_data;
    int32_t res;

    bool operator>(const StagedCqe& other) const noexcept {
        if (complete_at_ns != other.complete_at_ns) {
            return complete_at_ns > other.complete_at_ns;
        }
        return seq > other.seq;
    }
};
```

Inside `IO`:
1. **`IO::NowNs()`**:
   ```cpp
   [[nodiscard]] uint64_t NowNs() noexcept {
       if (sim_ != nullptr) [[unlikely]] {
           return sim_->NowNs();
       }
       if (cached_now_ns_ == 0) {
           cached_now_ns_ = detail::MonotonicNowNs();
       }
       return cached_now_ns_;
   }
   ```
2. **`IO::PopReady()`**:
   - When `sim_ != nullptr && sim_->Config().chaos_scheduler && ready_queue_.Size() > 1`, pick a random index `idx = sim_->Rng().Bounded(ready_queue_.Size())`, swap `ready_queue_[idx]` with the front element, and pop it.
3. **`IO::Syscall()`**:
   - When `sim_ != nullptr`:
     - Check fault injection (`sim_->MaybeInjectError(op_name)`).
     - Stamp `op.sim_complete_at_ns = sim_->NowNs() + sim_->SampleLatencyNs()`.
4. **`IO::SubmitOrWait()` & `IO::DrainCompletions()`**:
   - Submit queued SQEs to `io_uring`, reap kernel CQEs into `sim_->StageCqe(...)`, advance `sim_now_ns_` to the earliest staged CQE or `TimerWheel` deadline, and wake only the fibers whose virtual completion time has arrived (`complete_at_ns <= sim_now_ns_`).

---

### 4.3. Overhead on Production Mode (`sim_ == nullptr`)

- `IO` gains a single pointer `sim::SimController* sim_ = nullptr;` (8 bytes).
- `Operation` needs no extra fields in production: `StagedCqe` stores `complete_at_ns` and `seq` inside `SimController`'s vector when `sim_ != nullptr`.
- The `if (sim_ != nullptr) [[unlikely]]` branch in `NowNs()`, `PopReady()`, `Syscall()`, and `Tick()` is statically predicted not-taken and stays in L1 instruction cache (always `nullptr` in production).

---

## 5. End-to-End Example: Deterministic WAL Crash & Timeout Testing

```cpp
TEST(WalDeterministicSimTest, SurvivesPowerCutAndTornWritesAcross100Seeds) {
    for (uint64_t seed = 1; seed <= 100; ++seed) {
        sim::SimConfig cfg{
            .seed = seed,
            .chaos_scheduler = true,
            .min_io_latency_ns = 10'000,
            .max_io_latency_ns = 100'000,
            .tail_spike_prob = 0.05,
            .torn_write_prob = 0.5,
        };
        sim::SimController sim(cfg);

        std::filesystem::path wal_dir = MakeTempDir();

        // Phase 1: Spawn concurrent writers, then pull the power plug
        {
            IoOptions opts;
            opts.buffer_pool = BufferPoolConfig{.slot_size = 4096, .slots = 64};
            IO io(0, opts, &sim);

            (void)io.Spawn([&](IO& rt) noexcept {
                // Real io_uring + real FixedBufferPool + real WAL segment files,
                // scheduled and timed deterministically by `sim`.
                ...
            });

            io.RunUntilDone();
            // Roll back or tear un-Fsync'd writes on the real segment files:
            sim.SimulatePowerCut();
        }

        // Phase 2: Recover from the real files on disk and verify invariants
        {
            IO recovery_io(0, IoOptions{}, &sim);
            ...
        }
    }
}
```

---

## 6. Implementation Plan

1. **Step 1 — `SimRng` & `SimController` Core (`src/tinyio/sim/sim_controller.hpp` / `.cc`):**
   - Implement `SimRng` (SplitMix64 + Xoshiro256++), virtual clock `sim_now_ns_`, and `StagedCqe` min-heap.
   - Add `RandomSwapFront` support to `detail::RingBuffer` for $O(1)$ chaos ready-queue selection.
2. **Step 2 — Event-Loop Interception in `IO`:**
   - Wire `sim::SimController* sim_ = nullptr` into `IO::NowNs()`, `IO::PopReady()`, `IO::Syscall()`, `IO::SleepForNs()`, `IO::SubmitOrWait()`, and `IO::DrainCompletions()`.
   - Add unit tests proving:
     1. Identical seeds produce identical fiber execution traces and completion orders across runs.
     2. `SleepForNs(60s)` and 60-second I/O timeouts execute in $< 1\text{ ms}$ of wall-clock time.
     3. Timeout vs. completion races (`9.9 ms` vs `10.1 ms` on a `10 ms` timeout) resolve 100% deterministically.
3. **Step 3 — Dirty Write Tracker & `SimulatePowerCut()`:**
   - Record un-`Fsync`ed writes in `SimController` on `Write` / `WriteFixed`, clear on `Fsync`, and corrupt/revert/tear on `SimulatePowerCut()` for WAL crash-consistency testing.
