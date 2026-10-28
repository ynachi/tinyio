# State of the Art: Write-Ahead Logging on tinyio

## Executive Summary

Building a high-throughput, low-latency Write-Ahead Log (WAL) on top of [`tinyio`](file:///Users/ynachi/codes/tinyio/src/tinyio/tinyio.hpp) requires moving past traditional database textbook patterns (multi-threaded mutex locks, OS page cache writebacks, blocking `fsync` syscalls).

`tinyio` presents a specific execution environment:
- **Thread-per-core (Shared-Nothing):** Each core runs a single OS thread. No cross-thread lock contention on the hot path.
- **Stackful Cooperative Fibers:** Assembly-optimized context switching (~15–30 ns) scheduled cooperatively without kernel preemption.
- **Modern Linux `io_uring`:** Defaulting to `SINGLE_ISSUER | DEFER_TASKRUN | TASKRUN_FLAG` with non-blocking event loops.
- **Hugepage-Backed Registered Buffers:** DMA directly between userspace memory and NVMe controller via [`FixedBufferPool`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/buffer_pool.hpp) and [`IO::WriteFixed`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/io_ops.cc#L137-L157), completely bypassing the kernel page cache and bounce buffers.

This document surveys the fundamental trade-offs, hardware realities, and architectural options for building production-grade WAL engines on top of `tinyio`. It is a **survey, not a decision**: §10 gives a recommended default and §11 lists what `tinyio` still lacks to implement the options.

### How to read this document

| If you want to…                                                    | Read     |
|:-------------------------------------------------------------------|:---------|
| Understand what the hardware and kernel actually guarantee         | §1       |
| See every independent design decision (the *axes*) in one table    | §2       |
| Compare whole-log layouts (segmented, ring, fixed-slot, zero-copy) | §3       |
| Pick a commit/group-commit protocol for fibers                     | §4       |
| Choose where the log physically lives (file, raw device, ZNS, …)   | §5       |
| Design record framing, checksums and crash recovery                | §6       |
| Understand segment lifecycle, recycling and retention              | §7       |
| Scale beyond one core, or put a Raft log on top                    | §8       |
| Know how to test it                                                | §9       |
| Get the recommendation and the missing `tinyio` APIs               | §10, §11 |

### Related in-tree documents

- [`docs/wal-rfc.hpp`](file:///Users/ynachi/codes/tinyio/docs/wal-rfc.hpp) — the sector-framed, ping-pong-buffer, dedicated-flusher design (Paradigm 1, Strategy 1B, dedicated flusher fiber).
- [`docs/hybrid-log-rfc.hpp`](file:///Users/ynachi/codes/tinyio/docs/hybrid-log-rfc.hpp) — the FASTER-style logical-address design (Paradigm 2).
- [`docs/rfc-deterministic-simulator.md`](file:///Users/ynachi/codes/tinyio/docs/rfc-deterministic-simulator.md) — fault injection (torn writes, `EIO`, un-`Fsync`ed rollbacks); the test strategy in §9 builds on it.
- [`docs/buffer-pool.md`](file:///Users/ynachi/codes/tinyio/docs/buffer-pool.md) — the fixed-buffer facility every design here leases from.
- [`src/tinyio/wal/`](file:///Users/ynachi/codes/tinyio/src/tinyio/wal/) — the in-progress implementation. Note that its [`wal.hpp`](file:///Users/ynachi/codes/tinyio/src/tinyio/wal/wal.hpp) (per-record `length|seq|salt|payload|crc` framing) and [`common.hpp`](file:///Users/ynachi/codes/tinyio/src/tinyio/wal/common.hpp) (4 KiB block headers + fragments) describe **two different framings**; §6 is meant to help choose one.

---

## 1. Ground Truth: Storage & Kernel Realities on `tinyio`

Every WAL design on `tinyio` is constrained by physical storage characteristics and Linux kernel internals:

```
  +-----------------------------------------------------------------------------------+
  |                          Physical NVMe Storage Layer                              |
  +-----------------------------------------+-----------------------------------------+
  | 1. Sector Granularity (4096 B / 512 B)  | O_DIRECT requires 4KB alignment in RAM  |
  |                                         | and file offsets. No partial writes.    |
  +-----------------------------------------+-----------------------------------------+
  | 2. Volatile NVMe Controller DRAM        | O_DIRECT is NOT durable by itself!      |
  |                                         | Requires NVMe FLUSH (fdatasync/RWF_SYNC)|
  +-----------------------------------------+-----------------------------------------+
  | 3. Hardware Atomicity (AWUN / AWUPF)    | Drives guarantee atomicity only up to   |
  |                                         | 4KB or 512B. A 64KB write can tear!     |
  +-----------------------------------------+-----------------------------------------+
  | 4. Filesystem Extent Allocation Jitter  | Extending a file, or first-writing an   |
  |                                         | unwritten extent, takes the fs journal. |
  |                                         | Preallocate AND zero-fill (see §1.4).   |
  +-----------------------------------------+-----------------------------------------+
```

### 1.1 The `O_DIRECT` Durability Myth
A common misconception in storage programming is that `O_DIRECT` guarantees durability because it bypasses the OS page cache. **It does not.**
Modern enterprise and consumer NVMe drives contain volatile controller DRAM write caches. An `O_DIRECT` write completion returned by `io_uring` only confirms that data reached the drive controller's internal write buffer. If power cuts at that instant, data in the controller cache is lost unless:
1. The drive has a functioning Power Loss Protection (PLP) capacitor backplane, or
2. An explicit durability barrier is issued to commit the controller cache to non-volatile flash. There are **three** ways to ask for one (see the table below).

| Durability primitive     | How it is expressed                                                                                                                                  | What the kernel sends to the NVMe drive                                                                                                            | When it is the right choice                                                      |
|:-------------------------|:-----------------------------------------------------------------------------------------------------------------------------------------------------|:---------------------------------------------------------------------------------------------------------------------------------------------------|:---------------------------------------------------------------------------------|
| **Separate flush**       | `IORING_OP_FSYNC` (`IORING_FSYNC_DATASYNC`) after the write; today's [`IO::Fsync`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/runtime.hpp) | `FLUSH` (empties the *whole* volatile cache, including other writers' data)                                                                        | Batches that span many writes/segments; the portable default                     |
| **Write with FUA**       | `O_DSYNC` on the fd, or `RWF_DSYNC` per write                                                                                                        | A single write carrying the Force-Unit-Access bit; **no separate FLUSH** if the filesystem can prove the write needs no metadata commit (see §1.4) | Small tail writes where one round trip beats two; the lowest-latency commit path |
| **Linked write + flush** | `IOSQE_IO_LINK` between `WRITE_FIXED` and `FSYNC`                                                                                                    | Same as "separate flush", but both SQEs go to the kernel in one `io_uring_enter` and the fiber parks **once**                                      | When FUA is unavailable but you want one park/wake instead of two                |

Practical consequences:
- **Detect, don't assume.** `cat /sys/block/nvmeXnY/queue/write_cache` says `write through` when the kernel believes no flush is needed (PLP or no volatile cache) and `write back` otherwise; `queue/fua` says whether FUA writes reach the device. `nvme id-ctrl /dev/nvmeX | grep vwc` shows the volatile-write-cache bit. A WAL that must run on mixed hardware should log these at startup.
- **PLP drives turn the barrier into (almost) a no-op**, so the commit cost collapses to one write latency. The "fsync wall" of §1.2 is a **non-PLP** (consumer / cloud-ephemeral) problem; do not tune group-commit parameters on a PLP drive and ship them to a non-PLP one.
- **Lying hardware exists.** Some drives acknowledge `FLUSH` without persisting. No design below can detect this; it is why replicated systems (§8) do not trust one disk.
- Today `IO::WriteFixed` takes no `rw_flags` and there is no way to link SQEs, so only the first row is expressible; see §11.

### 1.2 The Fsync Wall & Group Commit Necessity
On non-PLP NVMe drives, an `fdatasync` (NVMe `FLUSH`) takes **30 µs to 150 µs** (and milliseconds on some consumer drives) depending on drive load and queue depth.
If an engine issues an `fdatasync` after every individual record write:
$$\text{Max Throughput} = \frac{1}{60\,\mu\text{s}} \approx 16,666 \text{ sync appends/sec per core}$$
To achieve 100k–500k+ appends/sec on a single core, the WAL **must batch multiple fiber append operations into a single I/O submit + sync cycle** (Group Commit).

Two numbers must not be confused:

- **Sync cycles per second** is bounded by device latency: $\approx 1 / (t_{write} + t_{flush})$ when one batch is in flight.
- **Records per second** is $\text{cycles/s} \times \text{records per batch}$, and is *also* bounded by bandwidth: $\text{BW} / (\text{record size} + \text{framing})$.

Worked example (illustrative): 100-byte records, 64 fibers waiting per batch, 60 µs per cycle → 16.6k cycles/s × 64 ≈ **1.06 M records/s**, but only ~68 MB/s of data. Conversely, at *low* load each cycle carries one 100-byte record padded to a 4 KiB sector: **~40× write amplification** (see §3, Paradigm 1). Throughput numbers in §10's matrix must be read through this lens: "IOPS" of a WAL is not "records/s".

For reference, the measured device ceiling in this project is ~133k 4 KiB random-read IOPS at QD 128 and ~12k at QD 1 (see `AGENTS.md`). **WAL write+flush numbers have not yet been measured on srv401**; any figure in this document without a measurement is an estimate, and WAL benchmarks must follow the same protocol (fresh pre-created file, interleaved A/B arms built with the same compiler, tinyio-vs-tinyio only).

### 1.3 Hardware Atomicity vs Multi-Block Torn Writes
NVMe hardware guarantees atomic writes only up to the drive's **AWUN** (Atomic Write Unit Normal) and **AWUPF** (Atomic Write Unit Power Fail) boundaries—typically 4096 bytes (or 512 bytes, or on some drives less than the logical block!).
A multi-block write (e.g. 64 KiB or 256 KiB) is **not atomic** across sudden power loss:
- The controller can persist blocks 0 through 7, but fail on block 8.
- Blocks can even be written out-of-order by the drive's internal Flash Translation Layer (FTL).
Therefore, recovery must rely on sector-level self-checksumming or monotonically ordered sector sequence numbers, not on the assumption of all-or-nothing multi-block DMA transfers.

Failure classes a WAL must distinguish (they need different responses; see §6.4):

| Failure                           | What happened                                                    | Example                                             |
|:----------------------------------|:-----------------------------------------------------------------|:----------------------------------------------------|
| **Torn write**                    | A single in-flight write persisted partially, only near the tail | Power cut during a 256 KiB flush                    |
| **Reordered persistence**         | Later blocks of a write persisted, earlier ones did not          | Block 12 present, block 11 absent                   |
| **Lost write**                    | Device acknowledged, data never persisted (or flush lied)        | Firmware bug / volatile cache lie                   |
| **Misdirected write**             | Data persisted at the wrong LBA                                  | Firmware bug                                        |
| **Bit rot / latent sector error** | Previously good, durable data later reads wrong or fails         | Media decay; **can occur in the middle of the log** |

**Atomic-write alternatives (a design option, not yet in the other sections).** Linux 6.11 added `RWF_ATOMIC` (torn-write protection): an `O_DIRECT` write of a power-of-two size up to the advertised atomic write unit, naturally aligned, either lands completely or not at all. Filesystem support arrived later (XFS, and ext4 with bigalloc, around 6.13; check `statx(STATX_WRITE_ATOMIC)` rather than assuming). Where available it allows e.g. 16 KiB or 64 KiB atomic log blocks, which could let a framing scheme drop per-sector CRCs for *torn-write detection* (CRCs remain valuable for bit-rot detection). Since `tinyio`'s floor is kernel 6.1 and srv401 runs 7.0, this should be an *optional capability*, probed at startup, with checksummed framing as the fallback.

The classic alternative, the **double-write buffer** (InnoDB), exists to protect *in-place page updates*. An append-only WAL does not need it except for **in-place tail sector rewrites** (Strategy 1B), which is exactly why that strategy is the risky one.

### 1.4 Filesystem Metadata Contention (`ext4` / `XFS` JBD2 Journaling) — and the `fallocate` Trap
When appending to a file past its existing end-of-file (EOF), the kernel takes the filesystem's inode lock and schedules metadata extent allocation transactions in the filesystem journal (e.g., `jbd2` in ext4).
In high-throughput direct I/O workloads, metadata journal flushes contend directly with data flushes, producing tail-latency spikes (reports range from several ms to hundreds of ms; measure on your own filesystem — the exact figure depends on journal mode and load).

**`fallocate()` alone is not enough.** `fallocate(mode=0)` allocates *unwritten* extents: blocks reserved but flagged "reads as zero". The **first** write to each such region must *convert* the extent to "written", which is a **metadata update that needs a journal commit** before an `fdatasync` can be considered durable. Consequences:
- The first pass over a freshly `fallocate`d segment is *slower and jittery* — the very latency spike preallocation was meant to avoid.
- `O_DSYNC`/FUA fast-path (§1.1) is only available for **pure overwrites** of already-written extents; on unwritten extents the filesystem falls back to write + journal commit.

The standard fix is to **pre-write the file with zeros once** (after `fallocate`, or instead of it), then `fsync` the file *and its directory*. Subsequent log writes are pure overwrites: no allocation, no extent conversion, no journal traffic. PostgreSQL does this (`wal_init_zero`), and RocksDB's `recycle_log_file_num` reuses written files for the same reason. Pre-zeroing costs one sequential write of the segment size at setup/recycle time; do it from a *background* fiber with throttling so it does not steal bandwidth from the commit path.

Other file-level rules:
- Open segments with `O_DIRECT` (and `O_DSYNC` if using the FUA path); `statx(STATX_DIOALIGN)` reports the real memory/offset alignment requirement.
- `fsync` the **directory** after creating, renaming or deleting a segment, or the file itself may vanish after a crash even though its data is durable.
- Prefer a filesystem the project can reason about (XFS or ext4 `data=ordered`); copy-on-write filesystems (btrfs, ZFS) make "overwrite in place" a lie and should be rejected at startup.
- Keep the log on its **own device or partition** if possible: a shared device means data-file writeback and checkpoints contend with commit latency (and cause the SSD to alternate between fast and slow modes noted in `AGENTS.md`).

### 1.5 What Happens When the Device Returns an Error
Since the 2018 "fsyncgate" incidents, the accepted rule is: **after an `fsync`/`fdatasync` (or a write) fails with `EIO`, the WAL cannot know which writes persisted, and the kernel may have marked dirty pages clean. Retrying the `fsync` can falsely succeed.** The WAL must treat any I/O error on the commit path as **fatal for that log**: latch the error (as [`docs/wal-rfc.hpp`](file:///Users/ynachi/codes/tinyio/docs/wal-rfc.hpp) Invariant 4 does), fail all current and future waiters, and let the process restart and run recovery (or, in a replicated system, step down and re-sync from peers). `ENOSPC` on a preallocated, zero-filled segment cannot occur on a pure overwrite and should only be possible when creating the next segment — another argument for creating segments ahead of time (§7).

### 1.6 The Cost of Padding: Sector Size Choices
Because `O_DIRECT` forces whole-sector writes, the logical block size sets both the minimum I/O and the padding waste for sparse commits.

| LBA format                                     | Padding waste for a lone 100 B commit | Notes                                                                                                                                                        |
|:-----------------------------------------------|:--------------------------------------|:-------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 4096 B (default)                               | ~40×                                  | Maximum throughput per I/O; hardware atomicity usually ≥ one sector                                                                                          |
| 512 B (if the namespace is 512-native or 512e) | ~5×                                   | `nvme format --lbaf` can switch some namespaces; on **512e** drives a 512 B write is a read-modify-write *inside the drive* and torn-sector exposure returns |
| Variable (raw-device or ZNS)                   | depends                               | See §5                                                                                                                                                       |

Check `nvme id-ns` for the supported LBA formats; this is a deployment decision that changes the framing math in §6.

---

## 2. The Design Space at a Glance: Nine Independent Axes

The four "paradigms" of §3 are *points* in a larger design space; they are not mutually exclusive alternatives for every decision. For example, "zero-copy `Writev`" (Paradigm 4) is orthogonal to "circular vs. linear addressing" (Paradigms 1/2), and a recycled-segment ring (§7) is a *file-lifecycle* choice that any layout can adopt. A concrete WAL picks one option on each axis:

| # | Axis                       | Options                                                                                                              | Section    |
|:--|:---------------------------|:---------------------------------------------------------------------------------------------------------------------|:-----------|
| A | **Address → disk mapping** | Rolling segment files · single circular ring · logical 48/64-bit address over segments · fixed-slot index arithmetic | §3         |
| B | **Substrate**              | File on ext4/XFS · raw block device · NVMe passthrough · ZNS / zoned · FDP / placement hints                         | §5         |
| C | **Staging & copy**         | Copy into registered buffer · scatter-gather zero-copy · write directly from network buffer                          | §3 (P4)    |
| D | **Durability primitive**   | Write + `fdatasync` · `O_DSYNC`/FUA · linked write+fsync · `RWF_ATOMIC` + flush                                      | §1.1, §1.3 |
| E | **Commit coordination**    | Turnstile leader · dedicated flusher fiber · decoupled `Append`+`Sync(lsn)` · pipelined multi-in-flight              | §4         |
| F | **Sync policy**            | Per-commit · group commit (adaptive) · interval ("everysec") · none (rely on replication) · per-record class         | §4.5       |
| G | **Tail handling**          | Pad & seal (1A) · in-place rewrite (1B) · alternating tail sectors (1C) · smaller LBA (1D)                           | §3 (P1)    |
| H | **Framing & integrity**    | Fragment/block (LevelDB) · slotted sector · per-record header · hash chain · epoch/salt · AEAD                       | §6         |
| I | **Recovery & reclamation** | Linear scan · binary-search tail · header-ring / checkpoint · delete / recycle / punch-hole                          | §6, §7     |

Two further axes sit *outside* a single log and are covered in §8: **sharding** (one log per core vs shared) and **consensus integration** (Raft suffix truncation, hard state, indexing).

---

## 3. Layout Paradigms for `tinyio`

Four architectural archetypes address different access patterns, latency budgets, and recovery constraints. They mostly differ on axes A, C and G.

```
+-----------------------------------------------------------------------------------+
|                        FOUR WAL ARCHITECTURAL PARADIGMS                           |
+-------------------+--------------------+--------------------+---------------------+
| 1. Pipelined      | 2. Virtual-Address | 3. Fixed-Slot      | 4. Vectorized       |
|    Segmented DMA  |    Paged Log       |    Matrix Log      |    Scatter-Gather   |
|    Log            |    (HybridLog /    |                    |    Log              |
|                   |    Circular Ring)  |                    |                     |
| (Double-buffered  | (FASTER, Aeron;    | (TigerBeetle       | (Zero-copy payload  |
|  O_DIRECT stream) |  ring: TigerBeetle)|  journal slots)    |  direct Writev)     |
+-------------------+--------------------+--------------------+---------------------+
```

---

### Paradigm 1: Pipelined Segmented DMA Log (Double/Triple-Buffered Direct I/O)

The classical high-throughput streaming WAL (RocksDB, Kafka, Redpanda). The log is an append-only sequence of pre-allocated files (`000001.wal`, `000002.wal`, e.g., 64 MiB each). The writer leases two or three [`FixedBuffer`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/buffer_pool.hpp) slots (e.g., 64 KiB – 256 KiB each).

```
  Fiber Appenders (Core 0)
        │
        ▼ (memcpy into RAM)
  [ Buffer A (Active) ] ──── Full / Sync Barrier ────► [ Buffer B (Flushing) ]
  (Packing in userspace)                                      │
                                                              ▼ io_uring WRITE_FIXED
                                                      [ NVMe Segment File ]
                                                              │
                                                              ▼ io_uring FSYNC (linked) or FUA
                                                      [ Non-volatile NAND ]
```

Why two buffers, not one: while buffer B is being DMA-read by the controller its bytes must not change, so appenders need a second buffer to keep staging. Three buffers allow **two** flushes in flight (see "pipelined flush" in §4.4) at the cost of more complex ordering.

#### The Sub-Sector / Tail Boundary Problem
When an application fiber requests an immediate durable flush (e.g. transaction commit), but the buffer is only partially filled (e.g., 1,500 bytes into a 4096-byte sector), `O_DIRECT` requires writing a full 4096-byte multiple. Four resolution strategies exist:

```
  Strategy 1A: Log-Structured Padding (Immutable Sectors)
  Sector N:   [ Record 1 (800B) ] [ Record 2 (700B) ] [ PAD / ZEROES (2596B) ]
  Sector N+1: [ Record 3 starts cleanly at offset 0 of next sector ]
  -------------------------------------------------------------------------------
  Strategy 1B: In-Place Tail Overwrite (Sector Rewriting)
  First Sync:  Sector N written to LBA X with [ Rec 1 ] [ Rec 2 ] [ Zeroes ]
  Second Sync: Sector N re-written to LBA X with [ Rec 1 ] [ Rec 2 ] [ Rec 3 ] [ Rec 4 ]
  -------------------------------------------------------------------------------
  Strategy 1C: Alternating Tail Sectors (A/B Tail)
  Sync k   : partial tail written to LBA X      (version k)
  Sync k+1 : partial tail written to LBA X+1    (version k+1, a superset of k)
  Sync k+2 : partial tail written to LBA X      (overwrites version k, never k+1)
  Recovery : read both, keep the valid one with the highest LSN
  -------------------------------------------------------------------------------
  Strategy 1D: Smaller LBA (512 B logical blocks)
  Same as 1A but the pad granule is 512 B, so a lone 100 B record wastes 412 B, not 3996 B.
```

* **Strategy 1A: Pure Log-Structured (Immutable Sectors with Padding)**
  - Unwritten bytes in the trailing 4096-byte sector are zero-padded (or stamped with a `PAD` record header).
  - The next batch starts cleanly at the next physical 4096-byte sector boundary.
  - **Pros:** Every sector is strictly immutable and written exactly once. Zero flash wear amplification from rewriting. Perfect for Zoned Namespaces (ZNS). Trivial recovery; no torn-overwrite hazard for acknowledged data.
  - **Cons:** Slack space waste (and extra bandwidth) when low-frequency writes call `Sync` constantly. Under high concurrent load, batches fill whole sectors anyway, making slack negligible. A **bounded commit delay** (§4.4) trades a few µs of latency for much better sector utilisation.

* **Strategy 1B: In-Place Sub-Sector Tail Rewrite**
  - The partial 4096-byte sector is flushed. When subsequent records arrive, the engine resumes packing in the *same* 4096-byte memory block and re-writes the *same LBA* on disk.
  - **Pros:** Zero wasted disk space.
  - **Cons:**
    1. **Flash Write Amplification:** The SSD's Flash Translation Layer cannot overwrite in-place; it marks the physical flash page stale, allocates a new page, and schedules garbage collection.
    2. **Torn Overwrite Hazards:** If power is lost during the second write, a torn sector write can corrupt `Record 1` and `Record 2`, which were already acknowledged to clients! This is only a risk if the drive does *not* guarantee power-fail atomicity for a full logical block (check AWUPF/`nawupf`, §1.3): on a drive that does, the sector is either the old or the new version, never a mix. Where it is a risk, it necessitates fine-grained per-fragment CRCs, an immutable prefix (as `docs/wal-rfc.hpp` does) and recovery logic to reconstruct valid prefixes — and it still cannot protect against a drive that tears *within* the old prefix.

* **Strategy 1C: Alternating Tail Sectors (A/B tail)**
  - Borrowed from LMDB's twin meta-pages. Each partial-tail flush goes to the *other* of two reserved LBAs, so the last acknowledged version is **never overwritten**; at worst the new write tears and recovery falls back to the previous version.
  - **Pros:** Removes the torn-overwrite hazard of 1B with no extra space beyond one extra sector, and still avoids padding every commit into a fresh sector.
  - **Cons:** The tail is not contiguous with the rest of the log, so recovery and the in-memory-window read path need a special case; the SSD still sees rewrites (two LBAs instead of one); a full sector must eventually be **sealed** into its final position (one more write), so a sync that completes a sector costs an extra I/O unless the seal write is itself the final version.

* **Strategy 1D: Smaller logical blocks** (see §1.6) — not a different algorithm but the cheapest way to cut 1A's waste on drives that support a 512 B LBA format.

---

### Paradigm 2: Virtual-Address Paged Logs (FASTER HybridLog vs. TigerBeetle-style Circular Ring)

Instead of managing directories of rolling files with manual offset tracking and parsing variable-length disk streams on recovery, this family of architectures virtualizes a continuous 64-bit (or 48-bit) **Logical Byte Address** space $[0, 2^{64})$ across an **in-memory sliding window** of [`FixedBuffer`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/buffer_pool.hpp) pages and **underlying NVMe storage**.

> [!NOTE]
> **FASTER is not a WAL.** Its HybridLog is the *record store itself* (with an in-place-updatable mutable region); it is cited here for the **address-space and watermark mechanics**, which transfer to a WAL. Likewise, **Aeron**'s log buffers are `mmap`'d term buffers (not `O_DIRECT`), cited for the ring discipline. The mechanisms below are borrowed; the products are not drop-in references.

```
 Logical Address Space [0, 2^64):
 ┌─────────────────┬──────────────────────┬──────────────────────┬─────────────────► Head (Append)
 │ Evicted on NVMe │ In-Memory Read-Only  │ In-Memory In-Flight  │ In-Memory Active│
 │ (Disk read req) │ (Served from RAM!)   │ (Flushing to disk)   │ (Mutable Tail)  │
 └─────────────────┴──────────────────────┴──────────────────────┴─────────────────┘
                   ▲                      ▲                      ▲
                   Evicted Watermark      Durable Watermark      Flushing Watermark
```

#### The In-Memory Sliding Window & Watermarks
- The logical address space is divided into logical pages of size $P = 2^m$ (e.g. 256 KiB, perfectly matching one `tinyio` `FixedBuffer` slot).
- In RAM, a circular buffer holds $C = 2^k$ page slots (e.g. 16 or 32 slots).
- Four moving watermarks govern the lifecycle (listed in address order from the tail):
  1. `Head`: Monotonically increasing byte offset where active fibers append in RAM.
  2. `Flushing`: Boundary of pages currently being written to NVMe via `io_uring_prep_write_fixed`.
  3. `Durable`: Boundary confirmed persistent on disk (post-barrier, §1.1). **Only bytes at or below `Durable` may be exposed to readers that make externally visible decisions** (replicate to a follower as *committed*, apply to the state machine): serving non-durable bytes from the window is fine for *speculative* readers only.
  4. `Evicted / HeadAddress`: Boundary where memory pages are unmapped/recycled for reuse. A page may be evicted only once it is both **durable** and **no longer pinned by a reader** (a follower RPC still copying out of it) — a pin count per slot, or an epoch scheme, is required; the sketch above omits it.

#### The Killer Advantage for Raft / Consensus on `tinyio` — and Its Fine Print
In a consensus system (like Raft), followers and local state machines frequently read recently committed entries. Because the in-memory window retains recent pages, **follower replication RPCs and state machine applies read directly from the in-memory buffer without issuing any disk I/O to NVMe!**

Fine print the original sketch omitted (all addressed in §8.2):
- A **lagging follower** falls off the window and forces cold reads through `ReadFixed`; the design needs that path anyway.
- Raft must **truncate a conflicting suffix** when a new leader overwrites uncommitted entries. A pure append-only/watermark design has no suffix truncation; it must be added deliberately.
- The window must be **sized against the fixed-buffer pool**: 32 × 256 KiB = 8 MiB per shard is small, but registered pages are pinned against `RLIMIT_MEMLOCK` (see `AGENTS.md`).

---

#### Deep Dive: Does the FASTER Style Have Only One Segment File on Disk?

**No. A common misconception is that FASTER's on-disk storage is a single circular file. It is not.**

There is a fundamental architectural split between **FASTER (Microsoft Research)** and **TigerBeetle / Aeron**:

```
                  LOGICAL BYTE ADDRESS SPACE [0, 2^48)
 ─────────────────────────────────────────────────────────────────────────────► (Grows forever)
 [ Evicted / Truncated ] [ Cold NVMe Pages ] [ In-Memory ReadOnly ] [ Active Head ]
                         │                   └──────────────┬─────────────────────┘
                         │                                  │
                         ▼                                  ▼
             ON-DISK SEGMENTS (Linear)            IN-MEMORY BUFFER (Circular)
       ┌──────────┬──────────┬──────────┐            ┌───┬───┬───┬───┬───┐
       │  seg.0   │  seg.1   │  seg.2   │            │ 0 │ 1 │ 2 │ 3 │...│
       │ (1 GiB)  │ (1 GiB)  │ (1 GiB)  │            └───┴───┴───┴───┴───┘
       └──────────┴──────────┴──────────┘             Slot = p(L) mod C
        seg_id = L / SegmentSize                      (Wraps around in RAM)
```

1. **In RAM (FASTER and Aeron-style rings):**
   Use a **single circular buffer** of $C$ slots that continuously wraps around modulo $C$:
   $$\text{RAM Slot Index} = p(L) \pmod C = (L \gg m) \ \& \ (C - 1)$$
   *(TigerBeetle is different: its ring is the **on-disk** journal, not an in-memory sliding window; it keeps only a small in-memory header cache and a bounded set of in-flight messages. Do not read the RAM-window advantages above as TigerBeetle properties.)*

2. **On Disk — FASTER (HybridLog):**
   - Disk addresses **do not wrap around**. The address space $[0, 2^{48})$ is a linear, monotonically increasing ray.
   - On disk, FASTER partitions this ray into **multiple numbered segment files** (`log.0`, `log.1`, `log.2`, ...), each holding $S = 2^s$ pages (e.g. 1 GiB per file):
     $$\text{Segment File ID} = \lfloor p(L) / S \rfloor = L \gg (m + s)$$
     $$\text{Offset Within Segment} = L \bmod (S \times P) = L \ \& \ ((1 \ll (m + s)) - 1)$$
   - **Why FASTER uses multiple segment files:**
     - **Trivial Space Reclamation:** As data is compacted or checkpointed, the `BeginAddress` watermark shifts forward. Reclaiming 100 GiB of disk space is a simple `unlink()` of older segment files. A single monolithic file would require punching sparse holes (`FALLOC_FL_PUNCH_HOLE`), which fragments extents and causes severe filesystem metadata lock contention.
     - **Infinite History & Cold Tiering:** Closed historical segments can be archived or tiered directly to object storage (Azure Blob / S3), while active segments remain on NVMe.
     - **No Ring-Wrap Contention:** If checkpointing lags, the writer does not collide with the tail on disk; it simply allocates another segment file.
   - **Caveat for a latency-sensitive WAL:** "simply allocates another segment file" and `unlink()` are exactly the metadata operations §1.4/§7 try to keep off the hot path. Combine this layout with **segment preallocation and recycling** (§7).

3. **On Disk — TigerBeetle-style ring:**
   - Disk addresses **do wrap around**. There is **strictly ONE pre-allocated file** (or a fixed, static ring of sectors/slots); in TigerBeetle this file also holds the superblock, the client-reply zone and the grid, so the WAL is a *zone* of it.
   - Disk offset is strictly:
     $$\text{Disk Offset} = L \bmod \text{RingCapacity}$$
   - **Why TigerBeetle uses a single circular ring file:**
     - **Zero Runtime File Operations:** No file creations, no file deletions, no inode modifications ever occur during runtime.
     - **Strict Resource Bounding:** Storage footprint is bounded at deployment time (e.g. exactly 10 GiB on disk).
     - **Strict Dependency on Checkpointing:** The write head must never overtake the checkpoint tail; if checkpointing lags, appends are backpressured.
   - **Redundant headers.** Because a wrapping ring contains old and new entries side by side, TigerBeetle keeps a **second ring of headers** next to the ring of payloads. A header says "slot *i* should hold op *n* with checksum *c*"; comparing the two rings classifies each slot as intact, **torn** (header written, payload not, or vice versa), or **corrupt** — and for a replicated system, a corrupt slot is *repaired from peers* rather than treated as the end of the log (see "Protocol-Aware Recovery", §6.4, §12).

#### Comparison: FASTER HybridLog vs. TigerBeetle Circular Ring

| Feature                        | **FASTER (HybridLog)**                                     | **TigerBeetle-style Ring**                                                |
|:-------------------------------|:-----------------------------------------------------------|:--------------------------------------------------------------------------|
| **On-Disk File Layout**        | **Multiple rolling segment files** (`seg.0`, `seg.1`, ...) | **Strictly ONE pre-allocated circular file/zone**                         |
| **Logical Address Space**      | Monotonically increases forever $[0, 2^{48})$              | Monotonically increases, but disk offset wraps: $L \bmod \text{Capacity}$ |
| **RAM Buffer**                 | Circular sliding window of page frames                     | Small in-flight set + header cache; no large read window                  |
| **Disk Space Reclaim**         | `unlink()` oldest segment files as `BeginAddress` advances | Overwrites oldest slots in place once checkpointed                        |
| **Max Capacity**               | Bound only by total disk capacity                          | Hard upper bound fixed at cluster deployment                              |
| **Failure Mode on Lag**        | Disk fills up (allocates more files)                       | System backpressures / halts (head cannot lap tail)                       |
| **Stale-data defence on wrap** | Not needed (addresses never reused)                        | Mandatory: per-slot op number / checksum / header-ring cross-check        |

---

### Paradigm 3: Fixed-Slot Matrix WAL (TigerBeetle Journal / High-Frequency Trading Model)

Classical WALs support arbitrary variable-length records (17 bytes, 312 bytes, 10,000 bytes). This requires complex byte-stream framing, length prefixes, fragmentation across block boundaries, and sequential recovery parsers.

The Fixed-Slot Matrix eliminates variable framing: **every log entry is a uniform fixed-size slot** (e.g., 256 bytes or 512 bytes), or an atomic sector containing exactly $K$ operations. (TigerBeetle's journal uses fixed *maximum-size* message slots — each prepare occupies one slot of the maximum message size, padded — rather than packing multiple tiny ops into a sector; both variants are valid points here.)

```
 Sector LBA:
 +-----------------------------------------------------------------------------+
 | Sector 0: [Header: LSN Base 100, CRC] | Op 100 (512B) | Op 101 | ... | Op 107 |
 +-----------------------------------------------------------------------------+
 | Sector 1: [Header: LSN Base 108, CRC] | Op 108 (512B) | Op 109 | ... | Op 115 |
 +-----------------------------------------------------------------------------+
```

* **How it works:**
  - Given an LSN $i$, its physical disk position is calculated in $O(1)$ without pointer chasing:
    $$\text{Sector Index} = \left\lfloor \frac{i \times \text{SlotSize}}{4096} \right\rfloor, \quad \text{Sector Offset} = (i \times \text{SlotSize}) \bmod 4096$$
    (For a ring: take the index modulo the slot count.)
  - Variable-size data (if any) is stored in a separate sidecar key-value blob store (WiscKey / Bitcask pattern), leaving the WAL purely for transaction metadata.
* **Why this is powerful on `tinyio`:**
  - **Out-of-Order Parallel Writes:** Multiple fibers can submit writes to their designated sectors concurrently using independent SQEs without serializing on a single buffer offset! **But:** acknowledging slot $i$ as durable is only safe once *every slot below $i$* is durable (otherwise recovery sees a hole and discards $i$, or worse, a consensus layer has acknowledged a non-prefix). The log therefore still maintains a **contiguous durable watermark**; parallel SQEs improve device queue depth, not the commit rule.
  - **Cheap Recovery:** Recovery doesn't scan the entire file sequentially. It binary-searches sector headers to locate the active tail in $\approx \log_2(\text{Total Sectors})$ reads (16 reads for a 256 MiB file) — that is $O(\log N)$, not $O(1)$; only *address lookup* by LSN is $O(1)$. See §6.5 for why the binary search needs a guard against out-of-order tears.
* **Costs and limits:**
  - **Internal fragmentation:** small ops in large slots waste space and write bandwidth; large ops need chunking or a sidecar.
  - **Fixed record size is a workload assumption** that is easy for a ledger and hard for a general-purpose store.
  - **Sidecar durability ordering:** if the payload lives in a blob store and the WAL holds a pointer, the blob must be durable *before* the WAL slot referencing it — two ordered durability steps, or one if both go to the same log.

---

### Paradigm 4: Vectorized Scatter-Gather Zero-Copy WAL (`Writev` Model)

In Paradigms 1–3, every `Append()` performs a `memcpy` from the user's buffer into the WAL's staging buffer. For small records, `memcpy` is cheap; for large state machine updates or batch ingest, `memcpy` consumes CPU bandwidth and pollutes L1/L2 caches.

```
 Stack / Fiber Memory:                User Payload Buffer:
 +---------------------------+        +-----------------------------------------------+
 | WalRecordHeader (16 Bytes)|        | Large Transaction Payload (e.g. 64 KiB)       |
 +---------------------------+        +-----------------------------------------------+
               │                                      │
               └───────────────┬──────────────────────┘
                               ▼
            std::array<iovec, 2> { &hdr, 16, &payload, 65536 }     ← NOT valid with O_DIRECT, see below
                               │
                               ▼
               IO::Writev (io_uring IORING_OP_WRITEV)
```

* **How it works:**
  - Construct an `iovec` array pointing to the record header, the user buffer and a padding tail.
  - Submit via [`IO::Writev`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/io_ops.cc#L169-L182).
* **The `O_DIRECT` constraint is stricter than the diagram above suggests.** On current kernels every `iovec` segment's *length* (and, depending on the device, base address) must be a multiple of the logical block size; a 16-byte header segment is rejected with `EINVAL`. So the header must be placed in a **full aligned sector segment** (header + padding, wasting up to 4 KiB per record) or **moved out of band** — e.g. into a separate metadata log, which leads straight back to Paradigm 3's sidecar pattern. A practical shape is:
  `[ aligned 4 KiB sector: header + start of payload ] [ aligned payload body ] [ aligned padded tail ]`.
* **Payload alignment from the network is the real obstacle.** Even with receive buffers from the [`FixedBufferPool`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/buffer_pool.hpp), a protocol header precedes the payload in the TCP stream, so the payload rarely lands at a sector-aligned address *and* a sector-aligned file offset. True zero-copy from socket to NVMe needs the protocol to be designed for it (fixed-size aligned frames, or "header-then-aligned-body" receive using `recv` into a pre-positioned buffer offset).
* **It is not zero-*touch*.** The per-record CRC still reads every payload byte once (CRC32C is a cheap ~10+ GB/s pass with hardware support, but it is not free); what is saved is the extra write into the staging buffer and the associated cache pollution.
* **`Writev` is not `WriteFixed`.** `IORING_OP_WRITEV` goes through the kernel's normal page-pinning path per call; fixed (registered) buffers avoid it. A hybrid — `IORING_OP_WRITE_FIXED` with a vectored variant (`IORING_OP_WRITEV_FIXED`, kernel 6.x+) — needs to be verified against the pinned liburing before relying on it.
* **Verdict:** a niche optimisation for large-record workloads. It composes with any other paradigm; do not pick it as a primary architecture.

---

## 4. Concurrency & Coordination Models within `tinyio`

Because `tinyio` runs a single OS thread per core, traditional multi-threaded concurrency primitives (`std::mutex`, `std::condition_variable`) do not apply. How do multiple fibers running concurrently on the same core coordinate appends and commits?

> Cooperative scheduling gives one free property: **between two suspension points a fiber is atomic**. Staging a record (assigning an LSN, copying bytes, checksumming) needs no lock as long as it never yields mid-way — which is exactly why §4.1's overflow case (a record bigger than the buffer must yield) needs an explicit admission rule.

```
               COOPERATIVE GROUP COMMIT (TURNSTILE) PIPELINE

  Fiber 1 (Leader)          Fiber 2 (Follower)          Fiber 3 (Follower)
        │                           │                           │
  AppendSync(rec1)            AppendSync(rec2)            AppendSync(rec3)
        │                           │                           │
  [Takes Flush Lock]        [Flusher active!            [Flusher active!
        │                    Pushes rec to Buffer B,     Pushes rec to Buffer B,
        │                    Parks on Baton]             Parks on Baton]
        ▼                                 │                           │
  Submits WriteFixed + Fsync              │                           │
  Yields to Event Loop                    │                           │
        │                                 │                           │
  ══════╪═════════════════════════════════╪═══════════════════════════╪══════
        │ (Kernel DMA in flight; Event loop runs other ready fibers)  │
  ══════╪═════════════════════════════════════════════════════════════╪══════
        ▼                                 │                           │
  CQE Completes!                          │                           │
  Wakes Waiters ──────────────────────────┴───────────────────────────┘
        │
  (Hands over leader role or flushes accumulated Buffer B)
```

### 4.1 The Cooperative Turnstile Group Commit (No Background Fiber)
- When Fiber 1 calls `AppendSync()`:
  - If no flush is in flight: Fiber 1 becomes the **Batch Leader**. It marks `flushing_ = true`, swaps the staging buffer, and initiates [`IO::WriteFixed`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/io_ops.cc#L137-L157) followed by [`IO::Fsync`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/io_ops.cc#L184-L188) (today two separate awaited calls; a *linked* pair or an `RWF_DSYNC` write would make it one, §1.1).
  - While Fiber 1 is suspended in `io_uring`, the tinyio event loop runs other fibers. Fibers 2 and 3 call `AppendSync()`. They see `flushing_ == true`, append their records into the standby buffer, register their [`Baton`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/fiber_sync.hpp#L62-L138) on a FIFO waiter queue, and park.
  - When the CQE returns, Fiber 1 resumes, unparks Fibers 2 and 3, and immediately flushes the batch that accumulated while it was waiting.
- **Why this is optimal:** Under zero load, latency is a single round trip (no sleep delays). Under heavy load, throughput automatically scales into large batches with zero timer overhead.
- **Weaknesses:**
  - **Leader latency coupling.** The leader is parked for its *own* flush *and* is expected to start the next one; if the application cancels or kills that fiber the batch machinery must hand over leadership (a "leader died" path, easy to get wrong).
  - **Unfair latency.** The leader pays for a flush that carries others' records; with shutdown/cancellation (`StopToken`) the leader's cancellation must not abandon followers' records.
  - **Error propagation.** A failed flush must wake every follower with the error (§1.5), not only the leader.

### 4.2 The Dedicated Flusher Fiber (Actor / Pipeline Pattern)
- Appender fibers never touch `io_uring`. They append records to an in-memory queue and sleep on a [`Baton`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/fiber_sync.hpp).
- A standalone `FlusherFiber` runs an event loop:
  ```cpp
  while (!stopping) {
      if (queue.empty()) {
          flusher_baton.Wait(); // Sleep until work arrives
      }
      auto batch = queue.Drain();
      io.WriteFixed(active_fd, batch.buffer);
      io.Fsync(active_fd, /*datasync=*/true);
      batch.NotifyCommitted(); // Posts all batons
  }
  ```
- **Trade-off:** Clean encapsulation, but introduces an extra fiber context switch (~30 ns) on every batch compared to the Turnstile Leader. In practice that cost is negligible next to a ≥10 µs device round trip; the real trade is *structure*: a dedicated fiber owns shutdown draining, error latching, segment rotation and background pre-zeroing in one place. This is the shape [`docs/wal-rfc.hpp`](file:///Users/ynachi/codes/tinyio/docs/wal-rfc.hpp) uses.
- **Shutdown note:** the flusher must be woken and drained by the runtime's shutdown path; a flusher left parked forever on its `Baton` trips `AbortIfParkedFibers()` (see `AGENTS.md`).

### 4.3 Decoupled Non-Blocking Append + Explicit Watermark (`Sync(target_lsn)`)
- Separate the concerns:
  - `Append(record) -> Lsn`: Completely non-blocking. Copies into the active buffer and returns the assigned LSN.
  - `Sync(target_lsn) -> void`: Suspends the calling fiber until disk persistence advances past `target_lsn`.
- **Why this matters for pipelining:** A fiber can append 100 log entries, trigger network replication RPCs to two Raft followers, and only call `Sync(100)` before responding to the client, allowing disk I/O and network RPCs to run in parallel.
- **Orthogonal to 4.1/4.2:** this is the *API shape*; it can sit on top of either a turnstile or a flusher fiber. The implementation is a monotonic `durable_lsn_` plus a FIFO (or LSN-ordered) waiter list. Make `Sync(lsn <= durable_lsn_)` a non-suspending fast path.
- **Edge cases worth specifying:** `Sync` on an LSN that was never appended (programmer error → `CHECK`); `Sync` after a latched error (returns the error immediately); and whether `Append` may be *rejected* when the buffer is full (back-pressure, §4.6).

### 4.4 Batching Policy: When Does a Flush Start?
The protocols above say *who* flushes. They leave open *when*, which determines the latency/throughput curve:

| Policy                        | Rule                                                                                   | Behaviour                                                                                | Used by                     |
|:------------------------------|:---------------------------------------------------------------------------------------|:-----------------------------------------------------------------------------------------|:----------------------------|
| **Immediate / opportunistic** | Flush as soon as the device is idle and anything is staged                             | Best latency at low load; batches self-size at high load (the Turnstile is this policy)  | Turnstile; many LSM engines |
| **Fixed commit delay**        | Wait up to $\Delta$ µs before flushing so more fibers can join                         | Better sector utilisation under moderate load; adds $\Delta$ to every commit at low load | PostgreSQL `commit_delay`   |
| **Adaptive delay**            | Delay only if the recent arrival rate suggests another fiber will join within $\Delta$ | Keeps low-load latency and moderate-load batching; needs a rate estimator                | Some commercial engines     |
| **Size / count threshold**    | Flush when staged bytes ≥ $B$ or records ≥ $N$, *or* a waiter is present               | Bounded buffer, bounded batch                                                            | Kafka (`flush.messages`)    |
| **Timer tick**                | Flush every $T$ ms regardless                                                          | Bounded loss window if used without `Sync`; wasteful if used *with* `Sync`               | "everysec" policies         |

On `tinyio` a delay is a `Sleep`/timer-wheel entry and costs a fiber park. Prefer **immediate/opportunistic** as the default and add an adaptive delay only after a benchmark proves sector under-utilisation matters.

**Pipelined flushes (more than one write in flight).** The designs above keep one write+barrier in flight and stage the next batch meanwhile (double buffering). With three or more buffers, batch $N+1$ can be *submitted* while $N$'s barrier is still pending, raising device queue depth. The price is ordering:
- `WRITE` ordering across in-flight batches is *not* guaranteed by `io_uring` unless linked (`IOSQE_IO_LINK`) or drained (`IOSQE_IO_DRAIN`), and a barrier only covers writes **completed before it is issued**.
- Durability must be **prefix-closed**: batch $N+1$ may be acknowledged only after batch $N$ is durable, even if its own barrier returns first. Keep a per-batch state machine (`staged → written → barrier-issued → durable`) and advance `durable_lsn_` strictly in order.
- Tail strategies 1B/1C (rewriting the same sector) are **incompatible** with pipelining, since two in-flight writes to one LBA have undefined persistence order. Pipelining requires 1A (immutable sectors).
- A failure of $N$ must fail $N+1, N+2, …$ (§1.5, and Invariant 4 of `wal-rfc.hpp`).

### 4.5 Sync Policy: How Strong Is "Committed"?
Durability is a product decision as much as a mechanism. A WAL should make the policy explicit and ideally per-append:

| Policy                          | Ack when                                            | Loss on crash                                                         | Typical use                                                            |
|:--------------------------------|:----------------------------------------------------|:----------------------------------------------------------------------|:-----------------------------------------------------------------------|
| **Sync every commit**           | After barrier completes (group commit)              | None                                                                  | Databases, ledgers                                                     |
| **Interval sync**               | Immediately; background barrier every $T$           | Up to $T$ of acked writes                                             | Caches, metrics, Redis "everysec", PostgreSQL `synchronous_commit=off` |
| **No local sync, replicated**   | After a **quorum** has the record in RAM or on disk | Safe only while a quorum survives; a correlated power loss loses data | Kafka defaults; some Raft deployments                                  |
| **Per-record durability class** | `Append(rec, Durability::{Sync, Async})`            | Per class                                                             | Mixed workloads (control records sync, bulk data async)                |

> [!WARNING]
> **Raft's safety proof assumes a node's acknowledged log and `currentTerm/votedFor` survive its own crash.** Acknowledging `AppendEntries` before the local barrier, or granting a vote without syncing `votedFor`, breaks that assumption: a power cut of a quorum can lose a committed entry. Skipping the local barrier is only defensible under a failure model that excludes correlated crashes (e.g. fault-isolated replicas with PLP) and must be an explicit, documented configuration, never a default.

### 4.6 Admission Control & Back-Pressure
A WAL with unbounded queues converts overload into memory growth and tail-latency collapse. Decide up front:
- **Staging buffer full:** park the appender (the `wal-rfc.hpp` approach — `EnsureBufferSpace()` + FIFO space waiters), or return `EAGAIN`.
- **Flush slower than ingest:** bound the number of *unflushed bytes* and the number of parked waiters; beyond that, shed load at the caller (reject with an error) rather than queueing.
- **Segment pool exhausted** (ring or recycle design): back-pressure is unavoidable; surface it as a distinct, observable condition (§9.3).
- **FIFO fairness** is cheap on a single thread and avoids a large record starving behind small ones — but note the head-of-line blocking it implies when one fiber is parked mid-record (Invariant 2 of `wal-rfc.hpp`).

---

## 5. Where the Log Lives: Substrate Options

Axis B is almost absent from typical WAL write-ups because they assume "a file". On `tinyio`, with `io_uring` and direct I/O, several substrates are realistic:

| Substrate                                                             | What changes                                                                                    | Pros                                                                                                                       | Cons                                                                                                                        | `tinyio` readiness                                                                    |
|:----------------------------------------------------------------------|:------------------------------------------------------------------------------------------------|:---------------------------------------------------------------------------------------------------------------------------|:----------------------------------------------------------------------------------------------------------------------------|:--------------------------------------------------------------------------------------|
| **Regular file, ext4/XFS** (default)                                  | Preallocated + zero-filled segments, `O_DIRECT [+O_DSYNC]`                                      | Portable, tooling (`ls`, backup, `df`), no privileges                                                                      | Filesystem metadata jitter (§1.4); FUA fast-path needs pure overwrites; extra layers                                        | Works today                                                                           |
| **Raw block device / partition**                                      | No filesystem; the log is LBA ranges you manage                                                 | No journal, no extent conversion, no `fsync` of directories, FUA/flush map directly; deterministic latency                 | You write allocation, naming, recycling, tooling; accidental overwrite risk; harder for ops                                 | Needs only `open()` on the device node; offsets/alignments must come from `BLKSSZGET` |
| **NVMe passthrough** (`IORING_OP_URING_CMD` on `/dev/ngXnY`)          | Submit NVMe `Write`/`Flush` commands directly with FUA, PRP/SGL, namespace-level flags          | Bypasses the block layer; per-command control (FUA, atomic-write units, ZNS append, FDP placement)                         | Needs `SQE128`-sized rings (a different ring setup than tinyio's current default), elevated privileges, drive-specific code | Not available; would require a ring-flag option in `IoOptions`                        |
| **ZNS (Zoned Namespaces)**                                            | Log is a sequence of **zones**, written only at the write pointer (zone append returns the LBA) | No FTL write amplification; sequential-only matches Strategy 1A; host controls garbage; reset-zone replaces delete/recycle | Few drives; zone-size/count limits; random reads fine, **in-place tail rewrites are impossible** (forces 1A)                | Needs zoned-block-device ioctls + `zone append` (via passthrough or `RWF_*`)          |
| **FDP / write hints** (`F_SET_RW_HINT`, NVMe Flexible Data Placement) | Tag the WAL stream as short-lived / distinct from data                                          | Keeps WAL blocks from sharing erase blocks with long-lived data → lower SSD write amplification, steadier latency          | Hardware/kernel dependent; advisory                                                                                         | Optional `fcntl` at open time                                                         |
| **Persistent memory / CXL**                                           | Persist with cache-line flush + fence instead of block I/O                                      | Sub-µs commits                                                                                                             | Not NVMe; different programming model; scarce hardware                                                                      | Out of scope for `tinyio`                                                             |

**Interaction with `io_uring` modes.** `IORING_SETUP_IOPOLL` (polled completions) can cut completion latency on NVMe with poll queues, but it requires `O_DIRECT` files, cannot be combined with sockets and timeouts on the same ring, and is generally unsuitable for a ring that also drives the network and timer wheel. A dedicated WAL ring would be a separate `IO` instance — consistent with thread-per-core only if that instance is owned by the same thread and driven from the same loop, which `tinyio`'s one-ring-per-`IO` model does not currently express.

**Registered files.** Using `IOSQE_FIXED_FILE` (registered file descriptors) removes a per-I/O fd lookup refcount; with a fixed small set of segment fds (current + next + recycled) this is a cheap, low-risk win once `tinyio` exposes file registration (the `SQPOLL` note in `runtime.hpp` already hints at it).

---

## 6. Wire Framing, Integrity & Crash Recovery

A WAL is only as good as its crash consistency guarantees. In a power cut, writes can tear mid-sector or across a multi-block batch.

```
                      SLOTTED SECTOR WIRE FRAMING (4096 B)
 +-------------------------------------------------------------------------------+
 | SectorHeader (32 B)                                                           |
 | [ Magic (4B) | Epoch (4B) | Sector LSN (8B) | Record Count (2B) | CRC32C (4B) |
 +-------------------------------------------------------------------------------+
 | Record 0: [Len (2B) | Type (1B) | CRC (4B) | User Payload (N Bytes)]          |
 +-------------------------------------------------------------------------------+
 | Record 1: [Len (2B) | Type (1B) | CRC (4B) | User Payload (M Bytes)]          |
 +-------------------------------------------------------------------------------+
 | ... Zeroed Slack / Padding ...                                                |
 +-------------------------------------------------------------------------------+
```

> [!NOTE]
> A whole-sector CRC in `SectorHeader` is incompatible with Strategy 1B (rewriting a partially filled sector changes the header's checksum while it is being rewritten, and a tear can invalidate already-acknowledged records). Slotted sector framing is therefore natural with **1A** (immutable, sealed sectors) or **1C**; with 1B use per-record CRCs and an immutable block header, as [`docs/wal-rfc.hpp`](file:///Users/ynachi/codes/tinyio/docs/wal-rfc.hpp) §2C does.

### 6.1 Framing Models Compared

| Framing Scheme                                                            | Slicing / Packing                                                                                                                                                           | Torn-Write Resilience                                              | Recovery Speed                   | Overhead                    |
|:--------------------------------------------------------------------------|:----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|:-------------------------------------------------------------------|:---------------------------------|:----------------------------|
| **LevelDB / RocksDB Style**                                               | Records sliced across **32 KiB** blocks (`FULL`, `FIRST`, `MIDDLE`, `LAST`); 7-byte header (CRC32 + len + type); a "recyclable" variant adds a 4-byte log number (11 bytes) | High (each slice carries length, type, CRC)                        | Linear scan from start           | Low (~7–11 bytes per slice) |
| **Slotted Sector Framing**                                                | Each 4KB sector starts with a `SectorHeader` containing epoch & sector LSN                                                                                                  | Maximum for immutable sectors (sector validated as an atomic unit) | Fast (can binary search sectors) | ~32 bytes per 4KB (~0.8%)   |
| **Per-record header, no blocks** (`src/tinyio/wal/wal.hpp` style: `length | seq                                                                                                                                                                         | salt                                                               | payload                          | crc`)                       | Records packed back-to-back, may straddle sector boundaries | Good if the CRC covers length+seq+salt; a torn length can make a scan read garbage (bound it by remaining segment size) | Linear scan | ~24 B per record |
| **Hash-Chained Log (Merkle Stream)**                                      | Record $i$ contains $\text{CRC}(Record_{i-1})$                                                                                                                              | Detects silent corruption, truncation, or spliced files            | Linear scan                      | 4–8 bytes per record        |
| **Cumulative checksum** (SQLite WAL)                                      | Each frame's checksum is a running checksum over all earlier frames, salted per generation                                                                                  | Detects reordering and stale frames; invalid frame ends the log    | Linear scan                      | ~24 B per frame             |

(An earlier revision described LevelDB/RocksDB blocks as 4 KiB; the real block size is 32 KiB. The *fragment-and-resync-at-block-boundary* idea is what matters: after a corrupt block a reader can skip to the next block boundary and resynchronise.)

Choosing between them is mostly about two questions: (a) *do records need to straddle sectors?* (if no — fixed-slot or sector-aligned records — framing gets much simpler, at the cost of padding), and (b) *do you need to resynchronise after mid-log corruption, or is any bad record the end of the log?* (§6.4).

### 6.2 Stale-Data Defence: Epoch / Salt / Generation
Any design that **reuses disk space** — recycled segments (§7), rings, ZNS-less overwrites — must ensure that old, valid-looking records are never mistaken for new ones. The mechanisms:
- **Segment epoch / salt** stored in the segment header and mixed into *every* record's CRC seed (as `docs/wal-rfc.hpp` and `src/tinyio/wal` both do). A record from the previous generation fails the CRC even though it is internally consistent.
- **Monotonic LSN continuity check:** the next record must carry `prev_lsn + 1`; this catches reordered persistence and phantom blocks (Invariant 5 of `wal-rfc.hpp`).
- **Zeroing the tear window** past the recovered tail at boot, so a block that persisted out of order cannot reappear after a second crash.
- This is *independent* of §7's "zero the first header" step: invalidating the *segment header* is what stops recovery from adopting a recycled file as live; the epoch/salt is what stops *stale data inside* a live segment from being replayed.

### 6.3 Optional Integrity Add-Ons

| Feature                      | Mechanism                                                                                                                       | Notes                                                                                                                                                                            |
|:-----------------------------|:--------------------------------------------------------------------------------------------------------------------------------|:---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Stronger checksum**        | CRC32C (hardware, 32-bit) is adequate for random corruption; XXH3-64 / 64-bit CRC reduce false-positive odds on very large logs | A 32-bit CRC has a $2^{-32}$ false-accept per corrupt record; across billions of records that is not negligible for an *archival* log, though irrelevant for a short-lived WAL   |
| **Encryption at rest**       | AEAD (AES-GCM / ChaCha20-Poly1305) per record or per sector; **nonce = (epoch, LSN)** is unique by construction                 | The auth tag *replaces* the CRC for integrity but still must be validated on every recovery read; keys and rotation live outside the WAL                                         |
| **Compression**              | Per batch before framing (LZ4/Zstd)                                                                                             | Shrinks I/O and padding; breaks fixed-slot indexing (Paradigm 3) and complicates in-place tails; compress *whole batches*, never individual records, for the CPU/ratio trade-off |
| **Record type / versioning** | 1-byte type; segment header carries a format version                                                                            | Needed for rolling upgrades; unknown types must be skippable or fail closed (decide explicitly)                                                                                  |
| **Padding records**          | Explicit `PAD` record type instead of raw zero slack                                                                            | Lets the reader distinguish "intentional pad" from "zeroed unwritten tail"                                                                                                       |

### 6.4 Recovery Policy: Torn Tail vs. Mid-Log Corruption
The single most important recovery question is: **when the scanner hits an invalid record, is that the end of the log, or damage in the middle?** Treating mid-log corruption as a torn tail *silently truncates acknowledged commits*; treating a torn tail as corruption refuses to start after every crash.

```
  Scan from start:   ✔ ✔ ✔ ✔ ✘ ... ?
                              │
                              └─ Is there any valid, current-epoch, higher-LSN data
                                 BEYOND the maximum in-flight window after this point?
                                    no  → torn tail: truncate here, resume appending
                                    yes → mid-log corruption: DO NOT truncate silently
```

Practical rule using the numbers already in the design: a crash can only leave invalid data within the **maximum in-flight write window** (one flush, or $k$ pipelined flushes; e.g. 256 KiB). After the first invalid block, **look ahead beyond that window** (and across sealed later segments). Valid current-epoch blocks that are further ahead than any in-flight write could explain prove the damage is *not* a torn tail.

Responses to mid-log corruption, in order of preference:
1. **Repair from a peer** (replicated systems): fetch the missing range from another replica and re-persist it — **Protocol-Aware Recovery** (Alagappan et al., FAST'18; implemented in TigerBeetle). A replica that cannot tell "never received" from "received and lost" must not vote or count towards quorum until repaired, or it can violate Raft's/VSR's safety.
2. **Fail stop with a diagnosis** (single-node systems): refuse to start; require an operator decision. Never serve from a log known to be missing acknowledged data.
3. **Salvage mode** (explicit, flagged): skip the damaged region to the next resync point (block boundary in LevelDB-style framing) and report loss. RocksDB exposes this as `WALRecoveryMode` (`kTolerateCorruptedTailRecords`, `kAbsoluteConsistency`, `kPointInTimeRecovery`, `kSkipAnyCorruptedRecords`); the same menu applies here.

### 6.5 Recovery Algorithms: Linear Scan vs. $O(\log N)$ Tail Search vs. Metadata

In traditional WALs, recovery reads the log from byte 0 to the end. For a 256 MiB file, that is 256 MiB of sequential reads on startup.

| Strategy                         | How                                                                                                 | Cost                                     | Caveats                                                                                                                                                                                                                                                                                                                                                                                     |
|:---------------------------------|:----------------------------------------------------------------------------------------------------|:-----------------------------------------|:--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Linear scan**                  | Read every record from the last checkpoint (or segment start)                                       | $O(\text{log since checkpoint})$         | Simple, validates everything, naturally finds mid-log corruption; bounded by checkpoint frequency                                                                                                                                                                                                                                                                                           |
| **Binary-search tail discovery** | Binary search over sector headers for the last valid sector                                         | $\approx\log_2 N$ reads (16 for 256 MiB) | **Only valid if "valid sectors form a prefix".** Out-of-order tears (§1.3) break that: a phantom valid block can lie beyond a gap, so the search can land past the true tail. Mitigate with LSN continuity from a known-good anchor, and by verifying a window of sectors around the candidate; **it does not detect mid-log corruption — run a full verify pass lazily in the background** |
| **Checkpointed tail hint**       | Periodically persist `durable_lsn` / tail position in a small side record (superblock, header ring) | $O(1)$ + scan from the hint              | The hint is only an *accelerator*: always scan forward from it, since the log may extend past a stale hint                                                                                                                                                                                                                                                                                  |
| **Header ring** (TigerBeetle)    | A separate ring of small headers, cross-checked with payloads                                       | One sequential read of headers           | Doubles write count per commit unless headers are batched; strongest classification of torn vs corrupt slots                                                                                                                                                                                                                                                                                |

Binary search is a nice cold-start optimisation; it must not be the *only* recovery path. A good default is **scan from the last checkpoint** (bounded) and use binary search only to resume appending quickly on a clean, large segment.

For the multi-segment case, recovery must also handle: an **empty newest segment** (created but never written), a **missing middle segment** (corruption, fail), a **segment older than the checkpoint** (ignore), and **two segments claiming the same sequence number** (a half-finished recycle, §7).

### 6.6 Idempotent Replay
Recovery may be interrupted and re-run, and a checkpoint may lag the data it describes. Replay must therefore be **idempotent**: either the records are *physical/redo-idempotent* (apply = set bytes at offset), or each applied record is guarded by the target's own LSN (`page_lsn >= rec_lsn → skip`, as in ARIES). A WAL library should hand the consumer the LSN of every record so it can implement this guard.

---

## 7. Segment Lifecycle: Creation, Recycling, Retention

In high-throughput logging, file management itself is a major latency bottleneck.

```
 The Naive File Lifecycle (Causes JBD2 Latency Spikes):
 [ Create New Segment ] ──► [ fallocate() ] ──► [ Write Data ] ──► [ unlink() Old Segment ]
            │                                                              │
            └────── Filesystem metadata journal (JBD2) lock stalls! ───────┘

 The Pre-Allocated Recycling Ring (Zero Runtime Metadata Cost):
 ┌─────────────────────────────────────────────────────────────────────────┐
 │ Slot Pool:  [ slot_0.wal ]  [ slot_1.wal ]  [ slot_2.wal ]  [ ... ]     │
 │ (fixed file names; the LOGICAL segment id lives in each file's header)  │
 └─────────────────────────────────────────────────────────────────────────┘
         ▲                                                              │
         └──────── Checkpointed slot re-stamped with a new header ──────┘
```

1. **The Extent Allocation Penalty:** Calling `open(O_CREAT)` + `fallocate()` + `unlink()` during runtime forces the filesystem (ext4/XFS) to write metadata transactions to its journal. Under high disk load, these journal transactions queue behind data flushes, causing large P99 latency spikes. As §1.4 explains, `fallocate` alone still leaves unwritten extents whose first write converts them — so the pool must also be **zero-filled** ahead of time.
2. **The Recycling Pattern:**
   - Pre-allocate and zero-fill a fixed pool of segment files at system setup, with **fixed names** (`slot_0 … slot_{K-1}`).
   - When logical segment $N$ is fully checkpointed and safe to discard, **do not unlink it**.
   - **Do not `rename()` it** (an earlier revision suggested renaming to segment $N+K$): a rename is a directory metadata operation that goes through the journal — exactly what the pool exists to avoid. Instead the file keeps its name, and the **logical segment id and a fresh epoch live in the segment header**; discovery at recovery time maps slots to logical segments by reading headers.
   - Re-stamping = overwrite the 4 KiB segment header with `{new logical id, new epoch}` and make it durable *before* any data of the new generation is written. The body is left as-is; stale blocks are rejected by the epoch seeded into every CRC (§6.2). No need to zero the whole body; optionally zero a small tear-window at the head of the new generation.
   - At runtime, rotating to a new segment is an $O(1)$ in-memory pointer swap plus one 4 KiB header write (and a durable barrier on it), with zero filesystem metadata operations.
3. **The Half-Recycled Crash:** a crash between "old data invalidated" and "new header durable" must leave a state recovery can classify. Rule: *write the new header first, durably, and only then treat the slot as the new generation*; recovery prefers the highest logical id and discards slots whose header is torn.
4. **Pool Sizing & Back-Pressure:** the pool must cover *retention*, not only checkpoint lag: slots needed = segments between the oldest retained LSN and the head. **Retention is a minimum over several consumers**:
   - the local checkpoint / state-machine snapshot,
   - the slowest Raft follower you are willing to catch up from the log (beyond that, ship a snapshot),
   - CDC / archival consumers,
   - an operator-set minimum history (e.g. for debugging or point-in-time recovery).
   If the pool is exhausted, either apply back-pressure (ring semantics) or **grow the pool** (create a new slot ahead of need, off the hot path).
5. **Create the next segment ahead of time.** Whatever the lifecycle, the next segment should be allocated, zero-filled, header-stamped and directory-fsynced by a background fiber well before the current one fills, so the commit path never allocates or fails with `ENOSPC`.
6. **Segment size trade-offs:** larger segments (256 MiB–1 GiB) mean fewer rotations and fewer headers but coarser retention, slower pre-zeroing, and slower linear recovery; smaller segments (16–64 MiB) give finer GC and faster per-segment recovery but more metadata and more rotation barriers. 64 MiB (the `src/tinyio/wal` default) and 256 MiB (`wal-rfc.hpp`) are both defensible; make it a configuration and test both.

**Space-reclamation options compared**

| Method                        | Metadata cost at runtime | Disk-space return      | Notes                                                          |
|:------------------------------|:-------------------------|:-----------------------|:---------------------------------------------------------------|
| `unlink()` old segment        | High (journal txn)       | Immediate              | Fine off the hot path in a background fiber; avoid during peak |
| **Recycle (re-stamp header)** | None (one 4 KiB write)   | None (space is reused) | Best for steady-state latency; needs pool sizing               |
| `FALLOC_FL_PUNCH_HOLE`        | High; fragments extents  | Immediate              | Avoid for hot logs                                             |
| Truncate (`ftruncate`) tail   | High                     | Immediate              | Only for removing a rolled-back suffix in a *non-ring* log     |
| Reset zone (ZNS)              | Device-side              | Immediate              | Natural fit for Strategy 1A                                    |

---

## 8. Beyond One Log: Sharding & Consensus Integration

### 8.1 One Log per Core vs. Shared Logs
`tinyio` is thread-per-core, which pushes the natural design towards **one WAL per shard (per core)**. That avoids all cross-core coordination, but raises the question of what the *global order* is, if any.

| Option                                 | How                                                                                                         | Pros                                                         | Cons                                                                                                                          | Seen in                                                                     |
|:---------------------------------------|:------------------------------------------------------------------------------------------------------------|:-------------------------------------------------------------|:------------------------------------------------------------------------------------------------------------------------------|:----------------------------------------------------------------------------|
| **Independent shard logs**             | Each core owns a log; no cross-shard ordering                                                               | No sharing, linear scaling; failure of one shard is isolated | No cross-shard atomicity; each shard's flush is its own barrier, so N shards issue N flush streams (watch device queue depth) | Seastar/Redpanda, Scylla commit log per shard, Kafka partitions             |
| **Two-phase commit across shard logs** | `PREPARE` record in each shard log, then `COMMIT`                                                           | Atomic multi-shard transactions                              | Extra round trips/barriers per cross-shard transaction                                                                        | Distributed databases                                                       |
| **Epoch-based group commit**           | Global epoch advances every ~10–40 ms; a transaction is durable when **all** shards have flushed that epoch | Cheap cross-shard ordering with a bounded latency floor      | Commit latency ≥ epoch length; needs a coordinator                                                                            | Silo, many OLTP research systems                                            |
| **Vector / distributed LSN**           | Each record carries dependencies as a vector of per-shard LSNs; recovery replays in a consistent cut        | No global point of serialisation                             | Complex recovery; metadata per record                                                                                         | Taurus (parallel logging)                                                   |
| **Shared log, single I/O core**        | One core owns the device and the log; other cores submit batches over SPSC queues                           | Single order, perfect batching                               | Cross-core messaging, a hot core, defeats shared-nothing                                                                      | Classic single-log engines (single-I/O-core variant is a tinyio adaptation) |

With independent shard logs, the device sees $N$ concurrent write+barrier streams. A `FLUSH` empties the *whole* volatile cache, so one core's barrier also waits for other cores' unflushed writes. With FUA writes (§1.1) a barrier is private to the write, which scales better under many-shard concurrency — a further reason to prefer the FUA path if available.

### 8.2 The Log as a Raft Log
A WAL that backs a Raft replica has requirements that a pure single-node WAL does not:

1. **Suffix truncation.** When a new leader's log conflicts, a follower must discard entries from index $i$ on. Options:
   - *Physical rewind:* set the tail back to $i$ and overwrite. Danger: stale entries at higher LSNs remain on disk and can be **resurrected by recovery after a second crash** unless the term/epoch is mixed into the record CRC and a *truncation barrier* (or the tear-window zeroing of §6.2) invalidates them. Ring/linear append-only designs (Paradigm 2) have no built-in truncation and must add one.
   - *Logical truncation record:* append a `TRUNCATE(i)` marker; recovery interprets it. Simple and crash-safe; costs scan complexity and defers space reclamation.
   - *Segment-per-term-boundary:* start a new segment at a conflict; discard the dead one entirely. Cheap but wasteful for frequent leader changes.
2. **Hard state.** `currentTerm` and `votedFor` must be durable **before** answering a vote or replicating in a new term. Store them in a tiny **separate file with two alternating slots** (generation number + CRC, like twin superblocks) written with FUA/barrier, or as a record type in the log. Do *not* rewrite it in place as a single sector.
3. **Index → offset mapping.** Raft addresses entries by index, followers ask "give me entries from $i$": either a **sparse in-memory index** (every $k$-th entry → segment/offset), rebuilt during recovery, or **fixed-slot arithmetic** (Paradigm 3: $O(1)$).
4. **Prefix truncation / snapshots.** After a state-machine snapshot, entries up to the snapshot index can be reclaimed (§7). The log must remember the snapshot's last included index/term (needed for the `prevLogIndex/prevLogTerm` consistency check on the first retained entry).
5. **Read-your-writes vs durable reads.** Followers receiving `AppendEntries` need the entries **durable before acknowledging**; the leader may replicate entries *before* its own barrier completes (parallel disk + network, §4.3), but may count its own vote toward the quorum only once durable.
6. **Throughput shape.** A leader pipelines `AppendEntries`; the WAL must expose a **batched read-from-index** API that serves from the in-memory window (Paradigm 2) when possible and falls back to `ReadFixed` for laggards.

For the full consensus-side reasoning see [`docs/the_raft_paper_i_wish_i_had_2026_systems_edition.md`](file:///Users/ynachi/codes/tinyio/docs/the_raft_paper_i_wish_i_had_2026_systems_edition.md).

---

## 9. Testing, Fault Injection & Observability

### 9.1 What to Test
A WAL's bugs live in crash windows, and a real power-cut test is slow and unrepeatable. Build the matrix around the deterministic simulator ([`docs/rfc-deterministic-simulator.md`](file:///Users/ynachi/codes/tinyio/docs/rfc-deterministic-simulator.md)):

| Fault                                 | Injection                                                                     | Property checked                                                                                 |
|:--------------------------------------|:------------------------------------------------------------------------------|:-------------------------------------------------------------------------------------------------|
| **Crash at every syscall boundary**   | Stop after the $k$-th submitted op, for all $k$                               | Recovered log contains **every acknowledged record** and **no un-ordered gap** (prefix property) |
| **Torn write**                        | Persist only a prefix (and a random subset) of an in-flight multi-block write | Recovery stops at a valid tail; acked records intact                                             |
| **Reordered persistence**             | Persist block 12 but not block 11                                             | Phantom-block defence (§6.2) holds across **two** consecutive crashes                            |
| **Lost un-synced writes**             | Drop every write not covered by a completed barrier                           | Acks are never issued before the barrier                                                         |
| **`EIO` on write / on fsync**         | Fail the $k$-th op                                                            | Error latched; no retry-after-fsync-failure; all waiters failed                                  |
| **Bit flip in the middle**            | Flip one bit in an acknowledged record                                        | Classified as mid-log corruption (§6.4), *not* silently truncated                                |
| **Interrupted recycle**               | Crash between header write and body use                                       | §7 "half-recycled" state classified correctly                                                    |
| **Cancellation / shutdown mid-flush** | `StopToken` while parked                                                      | No parked-fiber abort; no lost waiter; no use-after-free of stack-allocated waiter nodes         |
| **Chaos fiber scheduling**            | Seeded random ready-queue order                                               | Admission invariants (Invariant 2 of `wal-rfc.hpp`) hold                                         |

Complement with **model-based testing** (compare against an in-memory reference log), **property tests** on the framing codec (round-trip, every truncation point, every single-bit corruption), and **fuzzing the recovery scanner** with random bytes (it parses untrusted-ish disk state and must never crash or read out of bounds). Published frameworks worth borrowing ideas from: ALICE and CrashMonkey (crash-consistency), and Jepsen-style external fault tests for the replicated case.

### 9.2 Physical Validation
The simulator cannot validate the hardware. Periodically run the *real* binary under: a power-cut rig or VM with a **dm-flakey / dm-log-writes** device (replays the exact write order and drops un-flushed data), `blkdiscard`-ed drives, and with the volatile write cache toggled (`nvme set-feature -f 6`) to confirm that the barrier is actually doing work.

### 9.3 Metrics to Export
Without these, group-commit tuning is guesswork:
- commit latency histogram (append → durable ack), broken down into *queue wait*, *write*, *barrier*;
- batch size in records and bytes; **sector fill ratio** (payload ÷ sector bytes written) — the direct measure of Strategy 1A's padding cost;
- flushes/s, barrier latency, in-flight depth;
- staging-buffer occupancy and parked-waiter count (back-pressure, §4.6);
- segment pool: free slots, rotation latency, pre-zero progress;
- recovery duration, bytes scanned, torn-tail bytes discarded, corruption events;
- device facts logged at startup: `write_cache`, `fua`, LBA size, AWUPF, filesystem and mount options.

---

## 10. Architectural Decision Matrix & Recommendation

> [!WARNING]
> The numeric ranges in the "Throughput" row are **estimates**, not measurements (see §1.2). In particular, a $1\text{M}+$ *IOPS* figure cannot be a *device-write* figure on the hardware used in this project, whose measured ceiling is ~133k 4 KiB IOPS; it can only be a *records-per-second* figure obtained by packing many records into each sector. Replace every number here with measurements from the tinyio-vs-tinyio WAL benchmark before using it to choose a design.

| Criterion                     | 1. Pipelined Segmented Log                                | 2. Virtual-Address / Circular Log                        | 3. Fixed-Slot Matrix Log                                       | 4. Vectorized Scatter-Gather                      |
|:------------------------------|:----------------------------------------------------------|:---------------------------------------------------------|:---------------------------------------------------------------|:--------------------------------------------------|
| **Primary Sweet Spot**        | General-purpose ACID WAL, RocksDB-like stores             | Raft / Consensus engines, in-memory caches               | High-frequency financial ledgers, deterministic state machines | Network-to-disk pipelines, large object streaming |
| **Records/s (estimate)**      | Very High (batch-packed)                                  | Very High (batch-packed)                                 | Very High if records are small and fixed                       | High for large records; bandwidth-bound           |
| **Device I/Os per commit**    | 1 write (+ barrier) per batch                             | 1 write (+ barrier) per batch                            | 1 write per batch; ring may add a header write                 | 1 vectored write; padding can double it           |
| **Tail Latency (P99)**        | Low (amortized group commit)                              | Low–Ultra-low (no allocation; steady in-memory window)   | Low and the most predictable (no framing parse)                | Variable (depends on iovec count and alignment)   |
| **Read / Catchup Speed**      | Cold NVMe read required                                   | **Instant for recent data (RAM window)**; cold otherwise | Direct index calculation                                       | Cold NVMe read required                           |
| **Flash Wear (WAF)**          | Medium (slack padding, 1A) to High (in-place rewrite, 1B) | Low (pure sequential ring)                               | Low (pure block writes)                                        | Low–Medium                                        |
| **Recovery Speed**            | Linear scan ($O(N)$) or bounded by checkpoint             | $O(\log N)$ tail search or checkpoint hint               | $O(\log N)$ tail search + $O(1)$ address lookup                | Linear scan                                       |
| **Suffix truncation (Raft)**  | Natural (rewind within segment, with guards)              | **Needs explicit design**                                | Natural (rewrite slot, new epoch)                              | As Paradigm 1                                     |
| **Payload Flexibility**       | Arbitrary variable length                                 | Arbitrary variable length                                | Uniform fixed records (or chunked / sidecar)                   | Variable length, with alignment constraints       |
| **Memory Footprint**          | 2–3 `FixedBuffer` slots                                   | 8–32 `FixedBuffer` slots                                 | 1–2 `FixedBuffer` slots                                        | Buffer pool for headers + user payload            |
| **Implementation Complexity** | Moderate                                                  | High (watermarks, pinning, eviction)                     | Low–Moderate (but workload-constrained)                        | High (alignment, ownership of user buffers)       |

### Recommended Default for `tinyio` (subject to measurement)
1. **Layout:** Paradigm 1 — segmented, immutable-sector log (**Strategy 1A**; Strategy 1C or 1B only if a measurement shows padding cost dominates and the drive's AWUPF supports it), with a **recycled, pre-zeroed slot pool** (§7). This is the combination that maximises recoverability and keeps the door open for ZNS and pipelined flushes.
2. **Coordination:** dedicated flusher fiber (§4.2) behind a decoupled `Append`/`Sync(lsn)` API (§4.3), opportunistic batching (§4.4), prefix-closed `durable_lsn_`.
3. **Durability primitive:** `RWF_DSYNC`/FUA when the device reports FUA and the segment is a pure overwrite, linked write+`fdatasync` otherwise; probe at startup (§1.1).
4. **Integrity:** per-record CRC32C seeded with the segment epoch, LSN continuity, torn-tail vs mid-log classification with a look-ahead beyond the in-flight window (§6.2, §6.4); fatal latch on any I/O error (§1.5).
5. **Scale-out:** one log per core, no cross-shard ordering unless a workload demands it (§8.1).
6. **Raft path:** add the in-memory read window (Paradigm 2 idea) on top, plus suffix truncation and a hard-state file (§8.2) — as an *extension* of the same on-disk format, not a second format.

Revisit this default if: the workload is fixed-size ledger-like (→ Paradigm 3), records are very large and arrive aligned (→ add Paradigm 4's gather writes), or the deployment can provide ZNS/raw devices (→ §5).

---

## 11. Gaps in `tinyio` Needed to Implement These Options

Verified against [`runtime.hpp`](file:///Users/ynachi/codes/tinyio/src/tinyio/runtime/runtime.hpp) at the time of writing:

| Needed for                                                                           | Missing / limited today                                                                | Suggested addition                                                                                                                                   |
|:-------------------------------------------------------------------------------------|:---------------------------------------------------------------------------------------|:-----------------------------------------------------------------------------------------------------------------------------------------------------|
| FUA / `RWF_DSYNC` / `RWF_ATOMIC` (§1.1, §1.3)                                        | `WriteFixed`/`Writev` take no `rw_flags`                                               | `rw_flags` parameter (or `WriteOptions` struct) forwarded to the SQE                                                                                 |
| Linked write + fsync, one park (§1.1, §4.1)                                          | No SQE-linking API; each op parks separately                                           | A `Submit(Chain{...})` or `IO::WriteFixedThenFsync(...)` helper using `IOSQE_IO_LINK`; define how a failure in link position $k$ cancels $k+1\ldots$ |
| Pipelined flushes (§4.4)                                                             | Fibers can already have multiple ops in flight, but no ordering primitive (`IO_DRAIN`) | Optional drain flag; or enforce ordering with fiber-level state machine                                                                              |
| Registered files (§5)                                                                | No `IOSQE_FIXED_FILE` support exposed                                                  | File registration + `FixedFile` handle type                                                                                                          |
| Directory `fsync`, `statx(STATX_DIOALIGN)`, `statx(STATX_WRITE_ATOMIC)` (§1.4, §1.3) | `Fsync(fd)` works on a directory fd, but no `Statx` wrapper                            | `IO::Statx` wrapper (or synchronous helper at startup)                                                                                               |
| `ReadFixed` for recovery/cold catch-up (§6, §8.2)                                    | Present; recovery needs multi-segment sequencing on top                                | Library code, no runtime change                                                                                                                      |
| NVMe passthrough / ZNS (§5)                                                          | No `IORING_OP_URING_CMD`; ring is not created with `SQE128`                            | Ring-flag option in `IoOptions` + a command-submission method                                                                                        |
| Deterministic crash tests (§9)                                                       | Simulator RFC is *Proposed*                                                            | Implement `SimController` fault hooks at `Syscall()`/`WriteFixed()`/`Fsync()`                                                                        |
| Background pre-zeroing without hurting commits (§1.4, §7)                            | No I/O priority / throttling facility                                                  | A token-bucket in the WAL layer (chunked zero-fill writes paced by timers)                                                                           |

---

## 12. Prior Art & References

- **TigerBeetle:** Deterministic storage engine with a pre-allocated circular journal of fixed-size slots, a redundant header ring, sector-level checksumming, a superblock quorum, and *protocol-aware* repair of corrupt journal entries from peers.
- **FASTER (Microsoft Research):** HybridLog architecture combining in-memory circular page buffers with append-only multi-segment disk storage; the source of the logical-address and watermark mechanics in Paradigm 2.
- **Aeron:** Term-buffer based log with a rotating set of equal-size terms (`mmap`'d); source of the ring discipline.
- **Redpanda / Seastar:** Thread-per-core append-only log architecture on Linux `io_uring`/`AIO` with explicit memory pinning and per-shard logs.
- **RocksDB / LevelDB:** 32 KiB-block record framing with `FULL`, `FIRST`, `MIDDLE`, and `LAST` fragments; recyclable log files (`recycle_log_file_num`); `WALRecoveryMode`.
- **PostgreSQL:** `wal_init_zero` / `wal_recycle`, `commit_delay` / `commit_siblings` group-commit tuning, `synchronous_commit` levels, segment recycling; ARIES-style `page_lsn` idempotent redo.
- **SQLite WAL:** salted, cumulative frame checksums for stale-frame detection.
- **InnoDB:** doublewrite buffer for torn page protection (relevant to in-place rewrites only).
- **LMDB:** twin meta pages (the model for Strategy 1C and hard-state slots).
- **WiscKey / Bitcask:** Key-value separation using fixed-slot metadata logging and out-of-band blob storage.
- **Silo (Tu et al., SOSP'13):** epoch-based group commit across cores. **Taurus (Xia et al., VLDB'21):** parallel logging with vector LSNs. **Aether (Johnson et al., VLDB'10):** early lock release and consolidated log buffer.
- **Protocol-Aware Recovery for Consensus-Based Storage (Alagappan et al., FAST'18)**, and **"Can Applications Recover from fsync Failures?" (Rebello et al., ATC'20)** plus the 2018 PostgreSQL "fsyncgate" thread: the basis for §1.5 and §6.4.
- **"All File Systems Are Not Created Equal" (Pillai et al., OSDI'14)** and **ALICE**: crash-consistency pitfalls that motivate directory `fsync` and zero-fill rules.
- **Linux:** `Documentation/block/writing-atomic` / `RWF_ATOMIC` (6.11+), `io_uring_enter(2)` (`IOSQE_IO_LINK`, `IOSQE_IO_DRAIN`, `IORING_OP_URING_CMD`), `statx(2)` (`STATX_DIOALIGN`, `STATX_WRITE_ATOMIC`), NVMe spec (AWUN/AWUPF, FUA, VWC).

> [!NOTE]
> Figures (latencies, kernel-version thresholds, filesystem support for `RWF_ATOMIC`) are quoted from memory of the cited sources and should be re-verified against the target kernel and drive (`srv401`: kernel 7.0) before they drive a decision.
