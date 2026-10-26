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
- **`SleepForNs(duration)`**: In production ([`io_ops.cc:277-294`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/io_ops.cc#L277-L294)), `SleepForNs()` prepares an `IORING_OP_TIMEOUT` SQE (`io_uring_prep_timeout`), which would block the kernel for real wall-clock time if waited on. In simulation mode, `SleepForNs(duration)` instead prepares an `IORING_OP_NOP` (`io_uring_prep_nop(sqe)`) stamped with `sim_complete_at_ns = sim_->NowNs() + duration.count()` (and maps its `0` CQE result to `-ETIME` before the `res == -ETIME` check in `SleepForNs()`). Because `IORING_OP_NOP` is a real SQE that completes in `0 ns` of wall-clock time in the kernel and is held in the CQE Staging Min-Heap until `sim_complete_at_ns`, a `30 s` sleep finishes in nanoseconds of wall-clock time while preserving `inflight_ops_` accounting and `CancelAndDrain()` shutdown cancellation.
- **Pre-submission fault injection**:
  - If the seeded PRNG decides an operation fails immediately with `-EIO` or `-ENOSPC`, `Syscall()` either returns that error directly or submits an `IORING_OP_NOP` whose staged CQE result is overwritten with `-injected_errno` at `sim_complete_at_ns`.
  - If the seeded PRNG decides a write is a **torn write** (or short read), `Syscall()` clamps the SQE length (e.g., from `4096` bytes to `512` bytes) before submitting the real SQE to `io_uring`.

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
   When a fiber prepares an SQE in `Syscall()`, `sim_` draws a virtual duration $\Delta t$ from the seeded PRNG (according to a configurable latency profile) and associates `(sim_complete_at_ns, sim_seq)` with the operation's `user_data`:
   ```cpp
   sim_->RegisterInflightOp(user_data, sim_->NowNs() + sim_->SampleLatencyNs(op_kind), is_self_completing);
   ```
2. **Self-Completing vs. Peer-Dependent Operations (in `SubmitOrWait()`):**
   Operations submitted to `io_uring` fall into two categories:
   - **Self-completing operations (`self_completing_inflight_`)**: Regular file/disk operations (`Open`, `Read`, `Write`, `ReadFixed`, `WriteFixed`, `Readv`, `Writev`, `Fsync`, `Fallocate`, `Ftruncate`, `Rename`, `Remove`, `Close`) and simulated `SleepForNs` (`IORING_OP_NOP`). These never depend on a future action by another fiber and always complete in the kernel within microseconds.
   - **Peer-dependent operations (`peer_dependent_inflight_`)**: Socket `Accept`, `Connect`, `Poll`, and socket/pipe `Read`/`Write` waiting on another fiber. Waiting unconditionally on `peer_dependent_inflight_` in the kernel would deadlock if the peer fiber is currently parked in userspace waiting for a virtual timer or staged CQE before writing to the socket.

   Accordingly, `SubmitOrWait()` in simulation mode drains all `self_completing_inflight_` operations via a barrier wait, while submitting `peer_dependent_inflight_` operations non-blockingly (`wait_nr = 0`) whenever runnable fibers, staged CQEs, or armed timers exist:
   ```cpp
   // Wait until every self-completing SQE in this batch has produced a kernel CQE:
   while (sim_->SelfCompletingInflight() > 0) {
       (void)io_uring_submit_and_wait(&ring_, sim_->SelfCompletingInflight());
       HarvestKernelCqesToStagingHeap();
   }
   if (sim_->PeerDependentInflight() > 0) {
       (void)io_uring_submit_and_get_events(&ring_);
       HarvestKernelCqesToStagingHeap();
   }
   ```
3. **Virtual-Time Release (in `DrainCompletions()`):**
   Once the real kernel CQEs are sitting in `staged_cqes_` (a min-heap ordered by `(sim_complete_at_ns, sim_seq)`):
   - When `ready_queue_` is empty, the event loop compares the top of `staged_cqes_` (`next_cqe_ns`) against the next `TimerWheel` expiration (`next_timer_ns`) and advances `sim_now_ns_ = std::min(next_cqe_ns, next_timer_ns)`.
   - Every staged CQE with `sim_complete_at_ns <= sim_now_ns_` is popped in exact `(sim_complete_at_ns, sim_seq)` order, writes `op->cqe_res`, decrements `inflight_ops_`, disarms `timer_wheel_`, and calls `PushReady(op->fiber)`.

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

#### Serializing Concurrent Overlapping Reads and Writes
What if two fibers issue concurrent operations to the **same** byte range in the same tick (or overlapping virtual intervals)?
- Within a single tick, SQEs are prepared in the deterministic order that `ResumeReadyFibers()` pops fibers. However, the kernel's NVMe or workqueue threads could execute two concurrently submitted `WRITE` SQEs to the same file offset in arbitrary order unless constrained.
- To guarantee 100% deterministic disk state under concurrent overlapping I/O, `SimController` supports two submission policies for self-completing file operations:
  1. **Immediate Submission with `IOSQE_IO_LINK` on Overlapping Ranges (Default):** Non-overlapping file operations submit in batch; if two in-flight operations target overlapping `[offset, offset + len)` ranges on the same file, `SimController` serializes their kernel execution order by `(sim_complete_at_ns, sim_seq)`.
  2. **Deferred Submission at `sim_complete_at_ns`:** Self-completing file SQEs are held in a pending submission min-heap until `sim_now_ns_` reaches `sim_complete_at_ns`, at which point they are submitted to `io_uring` and reaped in exact `(sim_complete_at_ns, sim_seq)` order. Under this policy, if Fiber A issues a slow read (`t = 0..55 µs`) and Fiber B issues a fast overwrite (`t = 20..45 µs`) to the same block, the kernel executes Fiber B's write at `t = 45 µs` and Fiber A's read at `t = 55 µs`.

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

In simulation mode, timeout vs. completion is a **pure integer comparison** between two virtual timestamps.

Note how `tinyio`'s [`TimerWheel`](file:///Users/ynachi/codes/tinyio/src/tinyio/detail/timer_wheel.hpp#L38-L55) quantizes deadlines: `TimerWheel::k_tick_ns` is `64 ms` (`64'000'000 ns`), and `NsUntilNext(now_ns)` returns the end of the target `64 ms` slot (`(cur + off + 1) * k_tick_ns - now_ns`), while `Expire(now_ns)` checks `op->deadline_ns <= now_ns` when advancing ticks.
1. Suppose at `sim_now_ns_ = 0`, a fiber calls:
   ```cpp
   auto res = io.ReadFixed(fd, buf, 4096, offset, 0, /*timeout=*/std::chrono::milliseconds(100));
   ```
2. `Syscall()` sets `op.deadline_ns = 100'000'000` (`100 ms`) and places the operation in `TimerWheel` slot `1` (which fires when `TimerWheel` advances to tick `2` at `128 ms`, or at exact `op.deadline_ns = 100 ms` if `SimConfig::exact_timer_deadlines` is enabled).
3. `SimController` draws a simulated kernel latency for this read from `SimRng`:
   - **Case A (`sim_complete_at_ns < timer_fire_ns`, e.g., `95 ms`):** When the event loop advances `sim_now_ns_`, the staged CQE is popped first at `sim_now_ns_ = 95 ms`, disarms the `TimerWheel`, and the fiber deterministically resumes with `4096` bytes read.
   - **Case B (`sim_complete_at_ns > timer_fire_ns`, e.g., `140 ms`):** The `TimerWheel` expires first. `ProcessExpiredTimers()` marks `op.timed_out = true` and calls `sim_->CancelStagedOp(user_data, sim_now_ns_)` (which moves the already-harvested CQE in `staged_cqes_` to `sim_now_ns_` with `res = -ECANCELED`, avoiding a racy kernel `IORING_OP_ASYNC_CANCEL` whenever the CQE is already staged). The fiber deterministically resumes on the very next tick with `ETIMEDOUT`.

---

## 3. Simulating Power Cuts, Torn Writes, and Un-`Fsync`ed Data Loss on Real Files

A key requirement for testing `tinyio::wal` is verifying crash recovery after a sudden power loss. How do we simulate a power cut on a **real Linux file** (`tmpfs` or `ext4`) without writing a fake in-memory `SimDisk`?

When a program writes to a real Linux file via `io_uring` (`WRITE` / `WRITE_FIXED`), the bytes land in the OS page cache immediately—so if we simply close the file descriptor without calling `Fsync()`, a reader opening that file on Linux would still see the un-`Fsync`ed bytes.

To model volatile drive caches and power cuts on a real file, `SimController` tracks a **Dirty Write Log** keyed by canonical file path / `(dev, ino)` (mapped from `fd` on `Open()`, so closing and re-opening an `fd` before `SimulatePowerCut()` does not lose track of unflushed writes or confuse recycled descriptor numbers):

```
┌────────────────────────────────────────────────────────────────────────────┐
│                Real Linux File + Dirty Write Tracker (Sim)                 │
├────────────────────────────────────────────────────────────────────────────┤
│ 1. On Write / WriteFixed(fd, buf, len, offset):                            │
│    - Resolve effective offset (if offset == -1, query lseek(fd, 0, SEEK_CUR│
│      before submission)                                                    │
│    - For each 512B/4KiB sector in [offset, offset + len):                  │
│      * If the sector has no recorded pre-image since the last Fsync(),     │
│        pread() and store its original post-Fsync bytes in baseline_sectors │
│    - Append the write record (offset, new_bytes) to uncommitted_writes     │
│    - Submit the real WRITE / WRITE_FIXED SQE to io_uring                   │
│                                                                            │
│ 2. On Fsync(fd, datasync):                                                 │
│    - Submit the real FSYNC SQE to io_uring                                 │
│    - Clear baseline_sectors and uncommitted_writes for that file           │
│                                                                            │
│ 3. On SimController::SimulatePowerCut():                                   │
│    - First restore all dirty sectors of each file to baseline_sectors      │
│      (the exact durable state as of the last completed Fsync())            │
│    - Replay uncommitted_writes in submission order through SimRng:         │
│      * Keep whole write (flushed by drive controller before power died)    │
│      * Drop write (lost in volatile drive DRAM cache)                      │
│      * Tear on 512B/4KiB sector boundary (apply only a subset of sectors)  │
└────────────────────────────────────────────────────────────────────────────┘
```

Recording the **first** post-`Fsync` pre-image per sector (`baseline_sectors`) plus the ordered `uncommitted_writes` list solves the multiple-overwrite problem: if a fiber overwrites the same WAL header block twice before calling `Fsync()`, reverting to `baseline_sectors` and selectively replaying `uncommitted_writes` can faithfully produce the state after the last `Fsync()`, the state after the first uncommitted write, a torn first/second write, or the state after both writes.

What this achieves:
- **During normal operation**, every `WriteFixed` and `ReadFixed` goes straight through `io_uring` on the real file using real registered `FixedBuffer`s.
- **When `Fsync()` completes**, `baseline_sectors` and `uncommitted_writes` for that file are cleared (`O(1)` clear).
- **When `SimulatePowerCut()` is called**, the simulator modifies the real file via `pwrite` to reflect the exact post-crash sector state and abandons the crashed `IO` instance.
- A new `IO` instance then runs `wal::Reader::Recover()` on that **exact real file** through real `io_uring`.

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

    /// Probability of tearing or dropping uncommitted writes on SimulatePowerCut().
    double torn_write_prob = 0.0;
};

}  // namespace tinyio::sim
```

---

### 4.2. Staged CQE Min-Heap & `inflight_ops_` Lifecycle Inside `SimController`

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
3. **`IO::Syscall()` & `IO::SleepForNs()`**:
   - When `sim_ != nullptr`:
     - Check fault injection (`sim_->MaybeInjectError(op_name)`).
     - In `SleepForNs(duration)`, prepare `io_uring_prep_nop(sqe)` instead of `io_uring_prep_timeout` and register `sim_complete_at_ns = sim_->NowNs() + duration.count()`.
     - For normal operations, register `sim_complete_at_ns = sim_->NowNs() + sim_->SampleLatencyNs()`.
4. **`IO::SubmitOrWait()` & `IO::DrainCompletions()`**:
   - Submit queued SQEs to `io_uring` and harvest kernel CQEs into `sim_->StageCqe(...)`.
   - **Important `inflight_ops_` invariant**: `inflight_ops_` is decremented when a CQE is popped from `staged_cqes_` and delivered to its fiber (not when it is harvested from the kernel CQ ring into `staged_cqes_`). This ensures `IO::IsQuiescent()` remains `false` while staged CQEs are still pending in virtual time.
   - Advance `sim_now_ns_` to the earliest staged CQE or `TimerWheel` deadline when `ready_queue_` is empty, and wake only the fibers whose virtual completion time has arrived (`complete_at_ns <= sim_now_ns_`).
5. **Shutdown Cancellation (`IO::CancelAndDrain()`)**:
   - When `CancelAndDrain()` runs on shutdown, in addition to submitting `IORING_ASYNC_CANCEL_ALL` for any peer-dependent ops still in the kernel, it calls `sim_->CancelAllStaged(sim_now_ns_)`, which sets `complete_at_ns = sim_now_ns_` and `res = -ECANCELED` on all entries in `staged_cqes_` so `DriveToQuiescence()` drains deterministically in a single tick.

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
