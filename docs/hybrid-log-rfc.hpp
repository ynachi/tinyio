/**
 * ============================================================================
 * RFC-HYBRIDLOG-2026: MICROSOFT FASTER HYBRIDLOG STORAGE ENGINE SPECIFICATION
 * ============================================================================
 *
 * Status:       Staff Architecture Decomposition & Header RFC (for tinyio::IO)
 * Scope:        PURE HYBRIDLOG ONLY (Logical Address Allocator, 3-Region
 *               Circular Page Buffer, Watermark Pipeline, Cold Disk Reader).
 *               No Hash Index or Key-Value layer is coupled to this engine.
 * Reference:    "FASTER: A Concurrent Key-Value Store with In-Place Updates"
 *               (Chandramouli et al., ACM SIGMOD 2018, Section 4.1 & 4.2)
 * Target:       C++23, tinyio::IO (io_uring WRITE_FIXED / READ_FIXED, O_DIRECT)
 * Checksum:     google/crc32c (SSE4.2+PCLMULQDQ / ARMv8 CRC32C+PMULL)
 * Concurrency:  Thread-per-Core Cooperative Stackful Fibers (Shared-Nothing)
 * Alignment:    8-Byte Record Alignment in RAM; 4096-Byte Sector LBA on NVMe
 *
 * ============================================================================
 * 1. CORE STATE, MATHEMATICAL EQUATIONS & DATA STRUCTURES
 * ============================================================================
 * In the FASTER paper (Section 4.1–4.2), `HybridLog` (called `PersistentMemoryMalloc`
 * in the reference system) is a standalone, append-Mostly + in-place-mutable
 * record log that virtualizes a 48-bit logical byte address space `[64, 2^48)`
 * across a fixed-size in-memory circular page buffer and pre-allocated NVMe
 * segment files.
 *
 * ----------------------------------------------------------------------------
 * 1.1. System Geometry Parameters & Coordinate Mapping Equations
 * ----------------------------------------------------------------------------
 * Let the HybridLog geometry be defined by four power-of-two shift constants:
 *   - L in [64, 2^48) : 48-bit monotonically increasing Logical Byte Address.
 *                       (Offsets [0..63] of Logical Page 0 are reserved so that
 *                       `L == 0` is unambiguously `kInvalidLogicalAddress`.)
 *   - P = 2^m         : Page size in bytes (default m = 18 -> P = 256 KiB,
 *                       matching one `tinyio::FixedBuffer` slot).
 *   - C = 2^k         : Number of page slots in the in-memory circular buffer
 *                       (total per-core RAM capacity M = C * P = 2^(k + m)).
 *   - S = 2^s         : Number of pages per on-disk segment file
 *                       (pre-allocated segment size F = S * P = 2^(s + m)).
 *   - B = 4096 = 2^12 : Physical NVMe O_DIRECT sector size in bytes.
 *
 * Given any 48-bit Logical Address `L`, every physical coordinate in RAM and on
 * NVMe is defined by O(1) branchless bit-shifts (ZERO division instructions):
 *
 *   (Eq. 1) Logical Page Index:
 *           p(L) = floor(L / P) = L >> m
 *
 *   (Eq. 2) Intra-Page Byte Offset:
 *           off(L) = L mod P = L & (P - 1)
 *
 *   (Eq. 3) Circular Buffer Slot Index in [0, C - 1]:
 *           slot(L) = p(L) mod C = (L >> m) & (C - 1)
 *
 *   (Eq. 4) On-Disk Segment File ID:
 *           seg_id(L) = floor(p(L) / S) = L >> (m + s)
 *
 *   (Eq. 5) Byte Offset Within Segment File:
 *           seg_off(L) = L mod (S * P) = L & ((1ULL << (m + s)) - 1)
 *
 *   (Eq. 6) O_DIRECT 4KB Sector-Aligned Read Offset & Sub-Sector Delta:
 *           lba_off(L)   = seg_off(L) & ~(B - 1)
 *           lba_delta(L) = seg_off(L) &  (B - 1)
 *
 * ----------------------------------------------------------------------------
 * 1.2. The Seven Monotonic Watermarks & Three HybridLog Regions
 * ----------------------------------------------------------------------------
 * The HybridLog state is governed by seven monotonic 48-bit byte offsets that
 * MUST satisfy the Fundamental HybridLog Inequality at every tick:
 *
 *   (Eq. 7) 64 <= BeginAddress
 *              <= SafeHeadAddress
 *              <= HeadAddress
 *              <= FlushedUntilAddress
 *              <= SafeReadOnlyAddress
 *              <= ReadOnlyAddress
 *              <= TailAddress
 *
 *   (Eq. 8) Circular Buffer Non-Overwrite Invariant:
 *           p(TailAddress) - p(SafeHeadAddress) < C
 *
 * Visual Layout of the Unified HybridLog Address Space:
 * +--------------------------------------------------------------------------+
 * |                  Unified 48-Bit Logical Address Space                    |
 * +-----------------------+-------------------------+------------------------+
 * |     STABLE / COLD     |   IMMUTABLE READ-ONLY   |      HOT MUTABLE       |
 * |    (On NVMe Disk)     |  (In-Memory + Flushing) |  (In-Memory Working)   |
 * +-----------------------+-------------------------+------------------------+
 * ^                       ^            ^            ^                        ^
 * BeginAddress  HeadAddress   FlushedUntil  ReadOnlyAddress        TailAddress
 *               (SafeHead)                  (SafeReadOnly)
 *
 *   1. Stable / Cold Region [BeginAddress, HeadAddress):
 *      - Flushed to NVMe segment files and evicted from the circular buffer.
 *      - Resolving an address `L` here returns `AddressRegion::COLD_ON_DISK`;
 *        reading the record requires calling `ReadColdRecord(L, out_buf)`
 *        (which issues 4KB-aligned `io_.ReadFixed()` calls).
 *      - Addresses `< BeginAddress` have been reclaimed by `TruncateBefore()`
 *        and return `AddressRegion::TRUNCATED_EXPIRED`.
 *
 *   2. Immutable Read-Only Region [HeadAddress, ReadOnlyAddress):
 *      - Resident in the in-memory circular buffer at `slots_[slot(L)]`.
 *      - Records here are 100% IMMUTABLE (`ReadOnlyRecordView` only). Any
 *        higher-level engine wishing to modify a record at `L < ReadOnlyAddress`
 *        must call `Allocate()` to append a new record at `TailAddress`.
 *      - Background `PageFlusherFiber` flushes `[FlushedUntilAddress,
 *        SafeReadOnlyAddress)` to NVMe via `io_.WriteFixed()` directly from
 *        `slots_[s].dma_buf` while CPU fibers concurrently read from the slot.
 *
 *   3. Hot Mutable Region [ReadOnlyAddress, TailAddress):
 *      - The newest `C_mutable` pages in the circular buffer.
 *      - Resolving `L >= ReadOnlyAddress` returns a `MutableRecordView`
 *        pointing directly into `slots_[slot(L)].dma_buf`, allowing in-place
 *        payload overwrites (`CommitInPlaceMutation()`) with zero log tail
 *        allocation and zero disk I/O.
 *
 *   Why Both `X` and `SafeX` Watermarks Exist (`ReadOnly` vs `SafeReadOnly`,
 *   `Head` vs `SafeHead`):
 *     - Raising `ReadOnlyAddress` immediately stops new callers from obtaining
 *       a `MutableRecordView` below `ReadOnlyAddress`. Once all in-flight
 *       mutations below `ReadOnlyAddress` finish, `SafeReadOnlyAddress` catches
 *       up and unlocks `PageFlusherFiber` to write those bytes to NVMe.
 *     - Raising `HeadAddress` immediately stops new callers from obtaining an
 *       in-memory `ReadOnlyRecordView` below `HeadAddress`. Once all active
 *       `PageReadPin` leases on pages `< HeadAddress` drop to `0`,
 *       `SafeHeadAddress` catches up and frees the circular buffer slots for
 *       reuse at `TailAddress`.
 *
 *   Watermark Lag Equations (evaluated when `TailAddress` opens page `p_tail`):
 *     Let `C = 1 + C_ro + C_mutable`, where `1` slot is the active tail page,
 *     `C_ro >= 2` slots form the Read-Only flush pipeline, and `C_mutable`
 *     slots form the hot mutable working set:
 *
 *     (Eq. 9)  TargetReadOnly(p_tail) = max(ReadOnlyAddress,
 *                                           (p_tail - C_mutable) * P)
 *     (Eq. 10) TargetHead(p_tail)     = max(HeadAddress,
 *                                           (p_tail + 1 - C) * P)
 *
 * ----------------------------------------------------------------------------
 * 1.3. Summary Matrix of Core State Vectors & Object Schemas
 * ----------------------------------------------------------------------------
 * +-------------------------+------------------------------------------------+
 * | State Vector / Schema   | Exact Role & Memory Geometry                   |
 * +-------------------------+------------------------------------------------+
 * | `slots_[0 .. C-1]`      | Circular array of `C = 2^k` descriptors. Each  |
 * | (`PageSlotDescriptor`)  | owns 1 registered `tinyio::FixedBuffer` (P B), |
 * |                         | `logical_page_id`, `PageSlotState`, `active_pins`,
 * |                         | `written_end_offset`, `flushed_sector_offset`. |
 * +-------------------------+------------------------------------------------+
 * | `cold_scratch_bufs_`    | Pool of registered `tinyio::FixedBuffer` slots |
 * | + `scratch_slots_`       | + `tinyio::Semaphore` for 4KB O_DIRECT         |
 * |                         | cold reads in `[BeginAddress, HeadAddress)`.   |
 * +-------------------------+------------------------------------------------+
 * | `segments_`             | Vector of `SegmentFileEntry` mapping `seg_id`  |
 * | (`SegmentFileEntry`)    | to pre-allocated O_DIRECT `tinyio::UniqueFd`.  |
 * +-------------------------+------------------------------------------------+
 * | `LogRecordHeader`       | 16B self-describing frame header (`alignas(8)`)|
 * | (In-Page Record Schema) | placed before every payload: `payload_capacity`|
 * |                         | (4B), `payload_size` (4B), `record_crc32c` (4B)|
 * |                         | `flags_and_kind` (2B), `epoch_or_ver` (2B).    |
 * +-------------------------+------------------------------------------------+
 * | `HybridLogSnapshotMeta` | 4096B sector-aligned persistence manifest for  |
 * | (Log Checkpoint Header) | recovering `[BeginAddress, FlushedUntilAddress)`|
 * +-------------------------+------------------------------------------------+
 *
 * ============================================================================
 * 2. ALGORITHMIC PIPELINE (6 CORE LOG ALGORITHMS)
 * ============================================================================
 *
 * ----------------------------------------------------------------------------
 * 2.1. Algorithm 1: `Allocate(payload_size)` & Circular Page Rotation
 * ----------------------------------------------------------------------------
 *   1. Compute `payload_cap = AlignUp8(payload_size)` and total 8B-aligned
 *      frame size `frame_bytes = sizeof(LogRecordHeader) + payload_cap`.
 *      Require `frame_bytes <= P`.
 *   2. Fast Path (Fits in Current Tail Page `p = p(tail_address_)`):
 *      - If `off(tail_address_) + frame_bytes <= P`:
 *          * Assign `record_addr = tail_address_`.
 *          * Advance `tail_address_ += frame_bytes`.
 *          * Update `slots_[slot(record_addr)].written_end_offset`.
 *          * Initialize `LogRecordHeader` at `slots_[slot(record_addr)] + off(record_addr)`
 *            and return `MutableRecordView`.
 *   3. Slow Path (Page Boundary Overflow: `off(tail_address_) + frame_bytes > P`):
 *      - Step 3a (Seal Current Page `p`):
 *        Zero-fill `[off(tail_address_), P)` in `slots_[slot(p)]`. If
 *        `P - off(tail_address_) >= sizeof(LogRecordHeader)` (16B), stamp a
 *        `LogRecordKind::PAGE_END_SENTINEL` header so sequential scanners know
 *        no further records exist in page `p`.
 *      - Step 3b (Advance `ReadOnlyAddress` Watermark via Eq. 9):
 *        Let `next_page = p + 1`. Compute `TargetReadOnly(next_page)` and
 *        transition newly covered pages from `ACTIVE_MUTABLE` to
 *        `READ_ONLY_DIRTY`, waking `PageFlusherFiber`.
 *      - Step 3c (Enforce Circular Buffer Non-Overwrite Invariant Eq. 8):
 *        While `next_page - p(safe_head_address_) >= C` or
 *        `slots_[slot(next_page)].state != FREE_UNALLOCATED`:
 *          * Call `WakeFlusher()` and `eviction_slots_->Acquire()` — a
 *            `tinyio::Semaphore` holding one credit per evictable circular
 *            slot — and re-check `p(tail_address_)` after resuming (Step 3d).
 *      - Step 3d (Multi-Fiber Wakeup Re-Check):
 *        After resuming from `eviction_slots_->Acquire()`, if `p(tail_address_) >= next_page`
 *        (another fiber already rotated the tail onto `next_page`), loop back
 *        to Step 2! Otherwise, initialize `slots_[slot(next_page)]` as
 *        `ACTIVE_MUTABLE` for `next_page`, set `tail_address_ = next_page << m`,
 *        and execute Step 2.
 *
 * ----------------------------------------------------------------------------
 * 2.2. Algorithm 2: `ResolveAddress(addr)` — 3-Region In-Memory Resolution
 * ----------------------------------------------------------------------------
 *   Given a 48-bit `LogicalAddress` `L`:
 *     - Case A (`L >= read_only_address_` and `L < tail_address_`):
 *       Return `AddressRegion::HOT_MUTABLE` + `MutableRecordView` pointing to
 *       `slots_[slot(L)].PagePtr() + off(L)`. Caller may mutate payload in-place
 *       up to `hdr->payload_capacity` and call `CommitInPlaceMutation()`.
 *     - Case B (`L >= head_address_` and `L < read_only_address_`):
 *       Return `AddressRegion::WARM_READONLY` + `ReadOnlyRecordView` pointing
 *       to `slots_[slot(L)].PagePtr() + off(L)`. Zero-copy CPU read! If caller
 *       must suspend on I/O while holding this view, it acquires `PinPage(L)`.
 *     - Case C (`L >= begin_address_` and `L < head_address_`):
 *       Return `AddressRegion::COLD_ON_DISK`. Caller must invoke
 *       `ReadColdRecord(L, out_buf)`.
 *     - Case D (`L < begin_address_` or `L >= tail_address_`):
 *       Return `AddressRegion::TRUNCATED_EXPIRED` (or `OUT_OF_BOUNDS`).
 *
 * ----------------------------------------------------------------------------
 * 2.3. Algorithm 3: `ReadColdRecord(addr, out_buf)` — O_DIRECT Sub-Sector Read
 * ----------------------------------------------------------------------------
 *   Physical NVMe Sector Layout of an 8-Byte Aligned Record at `L`:
 *   +------------------------------------+-----------------------------------+
 *   |       NVMe 4KB Sector `K`          |      NVMe 4KB Sector `K + 1`      |
 *   +------------------+-----------------+-----------------+-----------------+
 *   | Prior Records    | LogRecordHeader | Payload Part 1  | Payload Part 2  |
 *   | [0 .. lba_delta) | (16B @ delta)   |                 |                 |
 *   +------------------+-----------------+-----------------+-----------------+
 *   ^                  ^
 *   lba_off(L)         seg_off(L) = lba_off(L) + lba_delta(L)
 *
 *   1. Lease one registered `FixedBuffer` from `cold_scratch_bufs_` via
 *      `scratch_slots_->Acquire()` (parking the fiber if all scratch slots
 *      are currently leased).
 *   2. Compute `seg_id(L)`, `lba_off(L)`, and `lba_delta(L)` via Eq. 4–6.
 *   3. Phase 1 Read (Header + First Sector):
 *      - Let `init_len = (lba_delta + sizeof(LogRecordHeader) > 4096) ? 8192 : 4096`.
 *      - Call `io_.ReadFixed(seg_fd, scratch_buf, init_len, lba_off, 0)`
 *        (suspends ONLY the calling fiber via `IO::Syscall()`).
 *   4. Phase 2 Read (Remaining Payload Sectors, if any):
 *      - Read `LogRecordHeader hdr` at `scratch_buf.Ptr() + lba_delta`.
 *      - Compute `total_frame = sizeof(LogRecordHeader) + hdr.payload_capacity`.
 *      - If `lba_delta + total_frame > init_len`:
 *          * Let `aligned_total = AlignUpSector(lba_delta + total_frame)`.
 *          * Call `io_.ReadFixed(seg_fd, scratch_buf, aligned_total - init_len,
 *                                lba_off + init_len, init_len)`.
 *   5. Checksum Verification & Output:
 *      - Recompute `ComputeLogRecordCrc32(hdr, payload_span)` and verify it
 *        equals `hdr.record_crc32c`.
 *      - Copy `[payload_ptr, payload_ptr + hdr.payload_size)` into `out_buf`.
 *
 * ----------------------------------------------------------------------------
 * 2.4. Algorithm 4: `PageFlusherFiberLoop()` — Zero-Copy DMA Page Flushing
 * ----------------------------------------------------------------------------
 *   +------------------------------------------------------------------------+
 *   |            Circular Buffer Page Slot Lifecycle State Machine           |
 *   +------------------------------------------------------------------------+
 *   | FREE_UNALLOCATED --> ACTIVE_MUTABLE --> READ_ONLY_DIRTY                |
 *   |        ^                                       |                       |
 *   |        | (TryEvictFlushedPages:                | (PageFlusherFiber:    |
 *   |        |  active_pins == 0, advances           |  io_.WriteFixed()     |
 *   |        |  HeadAddress & SafeHeadAddress)       v  to NVMe Segment)     |
 *   |        +------------ FLUSHED_CLEAN <--- FLUSH_IN_FLIGHT                |
 *   +------------------------------------------------------------------------+
 *
 *   0. Sleeping (when `flushed_until_address_ >= safe_read_only_address_`):
 *      Re-check the predicate FIRST, then create a fresh one-shot
 *      `tinyio::Baton` on the flusher's stack, publish it through
 *      `flusher_wake_`, and `Wait()` on it. A `WakeFlusher()` that lands
 *      while the flusher is inside `io_.WriteFixed()` (step 1) finds
 *      `flusher_wake_ == nullptr` and is dropped — not a lost wakeup,
 *      because the writer advanced `safe_read_only_address_` before waking,
 *      so the re-check sees the work. Shutdown is the same path: set
 *      `shutting_down_`, call `WakeFlusher()`; the loop condition ends the
 *      fiber with nobody left parked for the runtime's quiesce abort.
 *   1. While `flushed_until_address_ < safe_read_only_address_`:
 *      - Identify page `p = p(flushed_until_address_)` and `slot = slots_[slot(p)]`.
 *      - Let `page_base = p << m` and `target_end = min(safe_read_only_address_, page_base + P)`.
 *      - Compute sector-aligned window `[start_off, aligned_end)` where
 *        `start_off = slot.flushed_sector_offset` and
 *        `aligned_end = AlignUpSector(target_end - page_base)`.
 *      - Transition `slot.state = FLUSH_IN_FLIGHT` and issue
 *        `io_.WriteFixed(seg_fd, slot.dma_buf, aligned_end - start_off,
 *                        seg_off(page_base) + start_off, start_off)`.
 *      - On completion:
 *          * Update `flushed_until_address_ = target_end`.
 *          * If `target_end == page_base + P` (full page flushed):
 *              - Set `slot.flushed_sector_offset = P` and `slot.state = FLUSHED_CLEAN`.
 *          * Else (mid-page flush via `ShiftReadOnlyToTail()`):
 *              - Set `slot.flushed_sector_offset = (target_end - page_base) & ~(SECTOR_SIZE - 1)`.
 *              - Keep `slot.state = READ_ONLY_DIRTY` so the remaining bytes of
 *                page `p` will be flushed once sealed!
 *          * Run `TryEvictFlushedPages()` (which `Release()`s eviction
 *            credits) and post the one-shot `Baton` of every `flush_waiters_`
 *            node whose `target` is now covered.
 *
 * ----------------------------------------------------------------------------
 * 2.5. Algorithm 5: `TryEvictFlushedPages()` — Pin-Safe Circular Eviction
 * ----------------------------------------------------------------------------
 *   1. While `p(safe_head_address_) < p(flushed_until_address_)`:
 *      - Let `h_page = p(safe_head_address_)` and `slot = slots_[slot(h_page)]`.
 *      - Evict only when circular buffer pressure requires the slot
 *        (`p(tail_address_) - h_page >= C - 1`) or `head_address_` was
 *        explicitly shifted past `h_page`:
 *          * Advance `head_address_ = max(head_address_, (h_page + 1) << m)`
 *            so no new `PinPage()` or `ResolveAddress()` can enter `h_page`.
 *          * If `slot.state != FLUSHED_CLEAN` or `slot.active_pins > 0`, break
 *            (when the last `PageReadPin` releases, `UnpinPage()` wakes the
 *            flusher/evictor to complete the transition).
 *          * Reset `slot.state = FREE_UNALLOCATED`, `slot.logical_page_id = UINT64_MAX`,
 *            advance `safe_head_address_ = (h_page + 1) << m`, and
 *            `eviction_slots_->Release()` so the next Allocate() can proceed.
 *
 * ----------------------------------------------------------------------------
 * 2.6. Algorithm 6: Sequential `Scan()` & Cold Segment `TruncateBefore()`
 * ----------------------------------------------------------------------------
 *   1. `Scan(from_addr, to_addr, callback)`:
 *      - Iterates page-by-page across `[from_addr, to_addr)`.
 *      - If page `p` is in memory (`(p << m) >= head_address_`), pins `slots_[slot(p)]`
 *        via `PinPage()` and walks `LogRecordHeader` frames directly in RAM.
 *      - If page `p` is cold (`(p << m) < head_address_`), reads full `P`-byte
 *        page into a scratch `FixedBuffer` via `io_.ReadFixed()` and walks frames.
 *      - Stops scanning a page upon encountering `LogRecordKind::PAGE_END_SENTINEL`
 *        or `off + sizeof(LogRecordHeader) > P`.
 *   2. `TruncateBefore(new_begin_addr)`:
 *      - Advances `begin_address_ = max(begin_address_, new_begin_addr)`.
 *      - Closes and unlinks (`io_.Remove()`) every segment file with
 *        `seg_id < seg_id(begin_address_)`.
 *
 * ============================================================================
 * 3. INPUTS AND OUTPUTS SPECIFICATION
 * ============================================================================
 *   1. Configuration Input (`HybridLogConfig`):
 *      - `data_dir`                : Directory for `hybrid_seg_<shard>_<id>.dat`.
 *      - `page_size_shift`         : `m` (`P = 2^m`, must equal `FixedBuffer` slot size).
 *      - `buffer_pages_shift`      : `k` (`C = 2^k` circular slots in RAM).
 *      - `readonly_pipeline_pages` : `C_ro >= 2` slots reserved for flush pipeline.
 *      - `segment_pages_shift`     : `s` (`S = 2^s` pages per segment file).
 *      - `cold_read_scratch_slots` : Number of `FixedBuffer`s for cold reads/scans.
 *
 *   2. Runtime Operation Inputs:
 *      - `Allocate(payload_size, epoch_or_ver, user_flags)`
 *      - `ResolveAddress(logical_address)`
 *      - `CommitInPlaceMutation(MutableRecordView, new_payload_size)`
 *      - `ReadColdRecord(logical_address, out_payload)`
 *      - `ShiftReadOnlyToTail()` / `WaitForFlushUntil(target_address)`
 *      - `TruncateBefore(new_begin_address)`
 *
 *   3. Runtime Outputs & On-Disk Wire Format:
 *      - In-Memory Views: `MutableRecordView`, `ReadOnlyRecordView`, `PageReadPin`,
 *        and `AddressResolution` tagged union.
 *      - On-Disk Segment Files (`hybrid_seg_<shard>_<seg_id>.dat`):
 *        Pre-allocated `F = S * P` byte files consisting of `S` contiguous
 *        `P`-byte pages, each containing 8B-aligned `LogRecordHeader + Payload`
 *        frames terminated by `PAGE_END_SENTINEL` (or zero-fill if `< 16B`).
 *      - On-Disk Snapshot Manifest (`hybrid_log_meta_<shard>.snap`):
 *        4096B sector-aligned `HybridLogSnapshotMeta` storing `[begin_address,
 *        flushed_until_address]` and geometry shifts with a 64-bit CRC barrier.
 *
 * ============================================================================
 * 4. AMBIGUITY CHECK: 8 HYBRIDLOG PAPER GAPS VS. ENGINEERING BASELINES
 * ============================================================================
 * +----+----------------------------------+-----------------------------------+
 * | #  | Missing Detail in FASTER Paper   | Engineering Baseline in This RFC  |
 * +----+----------------------------------+-----------------------------------+
 * | 1  | Logical Address `0` Sentinel:    | Reserve `kInvalidLogicalAddress = |
 * |    | Paper starts log at byte 0 while | 0`. Zero-fill `[0..63]` of Page 0 |
 * |    | using 0 as NULL address.         | and start watermarks at `64`.     |
 * +----+----------------------------------+-----------------------------------+
 * | 2  | Page-Boundary Straddling:        | Enforce `frame_bytes <= P`. When  |
 * |    | Equations treat log as flat, but | `off(Tail) + frame_bytes > P`,    |
 * |    | slot `C-1` wraps to slot `0` in  | write `PAGE_END_SENTINEL` (16B)   |
 * |    | virtual memory.                  | and allocate at offset 0 of `p+1`.|
 * +----+----------------------------------+-----------------------------------+
 * | 3  | Cold Read O_DIRECT Alignment &   | Prefix every record with 16-byte  |
 * |    | Missing Record Framing/CRC:      | `LogRecordHeader` (`payload_cap`, |
 * |    | `L` is 8B-aligned, not 4096B-    | `payload_size`, `record_crc32c`). |
 * |    | aligned, and paper omits lengths | Align cold reads to `lba_off(L)`  |
 * |    | and checksums on disk.           | and parse at `lba_delta(L)`.      |
 * +----+----------------------------------+-----------------------------------+
 * | 4  | Variable-Size In-Place Updates:  | Separate `uint32_t payload_cap`   |
 * |    | Paper assumes fixed-size records | (immutable 8B-aligned allocation) |
 * |    | in the Mutable region.           | from `uint32_t payload_size`.     |
 * |    |                                  | Caller can shrink/grow up to      |
 * |    |                                  | `payload_cap`, or mark `INVALID`  |
 * |    |                                  | and re-allocate at `TailAddress`. |
 * +----+----------------------------------+-----------------------------------+
 * | 5  | Mid-Page `ShiftReadOnlyToTail()` | Track `flushed_sector_offset` per |
 * |    | Partial Flushing & Eviction:     | slot. A mid-page flush writes     |
 * |    | Paper omits what happens when    | 4KB-aligned sectors up to         |
 * |    | `ReadOnlyAddress` folds over at  | `SafeReadOnlyAddress` but keeps   |
 * |    | a non-page-aligned `TailAddress`.| `state = READ_ONLY_DIRTY` (never  |
 * |    |                                  | `FLUSHED_CLEAN`) until offset `P` |
 * |    |                                  | is reached, preventing eviction!  |
 * +----+----------------------------------+-----------------------------------+
 * | 6  | Concurrent Fiber Page-Rotation   | After waking from                 |
 * |    | Wakeup Race in `Allocate()`:     | `eviction_slots_->Acquire()`,     |
 * |    | Two fibers overflowing page `p`  | `if (p(tail_address_) >= p + 1)`  |
 * |    | both wake when slot `s(p+1)`     | and retry bump allocation instead |
 * |    | becomes free.                    | of double-initializing the slot.  |
 * +----+----------------------------------+-----------------------------------+
 * | 7  | Holding Read-Only Views Across   | Provide RAII `PageReadPin`        |
 * |    | Fiber Suspension Points:         | (`slots_[s].active_pins`).        |
 * |    | A fiber yielding or doing socket | `HeadAddress` advances immediately|
 * |    | I/O while reading a Warm page    | while `SafeHeadAddress` and slot  |
 * |    | could have its slot evicted.     | reuse wait for `active_pins == 0`.|
 * +----+----------------------------------+-----------------------------------+
 * | 8  | VFS `i_rwsem` Contention on      | Pre-allocate every segment file   |
 * |    | Extending Segment Writes:        | to `F = S * P` bytes on open via  |
 * |    | Extending file size during       | `io_.Fallocate()` so `WriteFixed` |
 * |    | `WriteFixed` serializes in VFS.  | is always an in-place overwrite.  |
 * +----+----------------------------------+-----------------------------------+
 *
 * ============================================================================
 * 5. HOW `tinyio` ENTERS THE PICTURE
 * ============================================================================
 *   1. `tinyio::FixedBufferPool` Is the Circular Page Buffer:
 *      `HybridLog::Start()` leases `C` slots of size `P` (e.g., `64 x 256 KiB`)
 *      from `io_.TakeFixedBuffer()`. Because `FixedBufferPool` maps 2 MiB
 *      huge pages (`MADV_HUGEPAGE`) and registers them with `io_uring`,
 *      `PageFlusherFiberLoop()` calls `io_.WriteFixed()` directly on
 *      `slots_[s].dma_buf` while CPU fibers read from the same Read-Only page
 *      with zero bounce buffers and zero kernel page-table walks.
 *
 *   2. Cooperative Stackful Fibers Simplify Epoch & Cold-Read Mechanics:
 *      - Synchronous in-memory accesses (`ResolveAddress` + `CommitInPlaceMutation`)
 *        never invoke a suspending `tinyio::IO` operation. Because `tinyio`
 *        fibers are cooperative, no other fiber on the core can preempt an
 *        in-memory access or evict a slot underneath it (`SafeReadOnlyAddress`
 *        tracks `ReadOnlyAddress` immediately).
 *      - Cold disk reads (`ReadColdRecord`) suspend the calling fiber's stack
 *        inside `io_.ReadFixed()` (`IO::Syscall()`) and resume transparently
 *        on the NVMe CQE — eliminating FASTER's heap-allocated `PendingContext`
 *        and manual `CompletePending()` polling loops.
 *
 *   3. Zero-Allocation Backpressure via tinyio's fiber primitives:
 *      - `PageFlusherFiberLoop()` sleeps on a fresh one-shot `tinyio::Baton`
 *        published through `flusher_wake_` (full contract next to the Baton
 *        in `tinyio/runtime/fiber_sync.hpp`): re-check the predicate before
 *        publishing, the poster clears the pointer with `std::exchange`
 *        BEFORE `Post()`, and nobody touches the baton after `Unpark()`.
 *      - `WaitForFlushUntil()` links stack nodes `{target, Baton, next}` into
 *        `flush_waiters_`; the flusher posts each node's Baton exactly once.
 *      - Slot eviction is a `tinyio::Semaphore` credit per evictable
 *        circular slot (`eviction_slots_`).
 *      - Scratch buffer pool for cold reads uses `tinyio::Semaphore`
 *        (`scratch_slots_`) for zero-allocation FIFO fair leases.
 * ============================================================================
 */

#pragma once

#include <fcntl.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <crc32c/crc32c.h>

#include "tinyio/check.hpp"
#include "tinyio/error.hpp"
#include "tinyio/logging.hpp"
#include "tinyio/net/fd.hpp"
#include "tinyio/runtime/buffer_pool.hpp"
#include "tinyio/runtime/fiber_sync.hpp"
#include "tinyio/runtime/runtime.hpp"

namespace storage::hybridlog {

// ============================================================================
// SECTION 1: GEOMETRY CONSTANTS & ADDRESS SPACE INVARIANTS
// ============================================================================

inline constexpr uint64_t HYBRIDLOG_MAGIC        = 0x4859424C4F473236ULL;  // ASCII "HYBLOG26"
inline constexpr uint32_t HYBRIDLOG_FORMAT_VER   = 1;

inline constexpr size_t   SECTOR_SIZE            = 4096;  // O_DIRECT LBA sector size (B = 2^12)
inline constexpr size_t   RECORD_ALIGN           = 8;     // In-page record alignment (8 bytes)

// Gap 1 Baseline: Logical Address 0 is reserved as NULL; Page 0 starts at byte 64.
inline constexpr uint64_t kInvalidLogicalAddress = 0;
inline constexpr uint64_t kInitialLogicalAddress = 64;

inline constexpr uint64_t kAddressBits           = 48;
inline constexpr uint64_t kAddressMask           = (1ULL << kAddressBits) - 1ULL;  // 0x0000FFFFFFFFFFFF

// Default geometry shifts matching `tinyio::FixedBufferPool` (256 KiB per slot).
inline constexpr uint32_t kDefaultPageShift      = 18;   // m: P = 2^18 = 256 KiB per page
inline constexpr uint32_t kDefaultBufferShift    = 6;    // k: C = 2^6  = 64 pages in circular buffer (16 MiB)
inline constexpr uint32_t kDefaultSegmentShift   = 10;   // s: S = 2^10 = 1024 pages per segment (256 MiB)

[[nodiscard]] constexpr size_t AlignUp8(size_t n) noexcept {
    return (n + (RECORD_ALIGN - 1U)) & ~(RECORD_ALIGN - 1U);
}

[[nodiscard]] constexpr size_t AlignUpSector(size_t n) noexcept {
    return (n + (SECTOR_SIZE - 1U)) & ~(SECTOR_SIZE - 1U);
}

// ============================================================================
// SECTION 2: IN-PAGE RECORD FRAME HEADER & HARDWARE CRC32C
// ============================================================================

/**
 * @brief Discriminator for records inside a HybridLog page (Gaps 2 & 4).
 */
enum class LogRecordKind : uint8_t {
    PAGE_END_SENTINEL = 0,  // Seals unwritten tail `[off, P)` when next record overflows page `p`
    NORMAL_RECORD     = 1,  // Live record containing `[0 .. payload_size)` bytes of user payload
    INVALID_SKIP      = 2   // Obsoleted/aborted record slot of length `16 + payload_capacity`
};

/**
 * @brief Self-describing 16-byte header placed at the start of every 8-byte
 *        aligned record frame in the HybridLog (resolves Gaps 2, 3, and 4).
 *
 * Horizontal Byte Framing in Page Memory & On-Disk Segment Files:
 * +--------------------------------------------------------------------------+
 * |               HybridLog Record Frame (8-Byte Aligned)                    |
 * +---------------+---------------+---------------+--------+--------+--------+
 * | payload_cap   | payload_size  | record_crc32c | kind   | flags  | epoch  |
 * | [0x00..0x03]  | [0x04..0x07]  | [0x08..0x0B]  | [0x0C] | [0x0D] | [0E..F]|
 * +---------------+---------------+---------------+--------+--------+--------+
 * |                  Payload Bytes [0 .. payload_capacity - 1]               |
 * |        (Active: [0 .. payload_size), Zero-Pad: [payload_size .. cap))    |
 * +--------------------------------------------------------------------------+
 *
 * Invariant:
 *   `payload_capacity` is ALWAYS a multiple of 8 (`AlignUp8(payload_size)`), so
 *   `TotalFrameBytes() = 16 + payload_capacity` is always 8-byte aligned with
 *   zero unaligned trailer bytes.
 */
struct alignas(8) LogRecordHeader {
    uint32_t payload_capacity = 0;  // [0x00..0x03] Immutable allocated payload capacity (multiple of 8)
    uint32_t payload_size     = 0;  // [0x04..0x07] Current active payload size (<= payload_capacity)
    uint32_t record_crc32c    = 0;  // [0x08..0x0B] Non-zero CRC32C of [0x00..0x07] + [0x0C..0x0F] + payload
    uint8_t  kind             = 0;  // [0x0C]       LogRecordKind
    uint8_t  user_flags       = 0;  // [0x0D]       Caller-defined bitmask (e.g., tombstone, compressed)
    uint16_t epoch_or_ver     = 0;  // [0x0E..0x0F] Caller-defined version / checkpoint epoch tag

    [[nodiscard]] static constexpr size_t TotalFrameBytes(uint32_t cap) noexcept {
        assert((cap & (RECORD_ALIGN - 1U)) == 0);
        return sizeof(LogRecordHeader) + static_cast<size_t>(cap);
    }

    [[nodiscard]] constexpr size_t TotalFrameBytes() const noexcept {
        return TotalFrameBytes(payload_capacity);
    }

    [[nodiscard]] constexpr LogRecordKind Kind() const noexcept {
        return static_cast<LogRecordKind>(kind);
    }
};
static_assert(sizeof(LogRecordHeader) == 16, "LogRecordHeader must be 16 bytes");

/**
 * @brief Hardware-accelerated CRC32C over `LogRecordHeader` (skipping the
 *        `record_crc32c` field at `[8..11]`) and the active `payload` bytes.
 */
[[nodiscard]] inline uint32_t ComputeLogRecordCrc32(
    const LogRecordHeader&     hdr,
    std::span<const std::byte> active_payload
) noexcept {
    const auto* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
    uint32_t    crc       = crc32c::Crc32c(hdr_bytes, 8);
    crc                   = crc32c::Extend(crc, hdr_bytes + 12, 4);
    if (!active_payload.empty()) {
        crc = crc32c::Extend(
            crc, reinterpret_cast<const uint8_t*>(active_payload.data()), active_payload.size());
    }
    return crc == 0 ? 1U : crc;
}

// ============================================================================
// SECTION 3: CIRCULAR PAGE SLOT & SNAPSHOT MANIFEST SCHEMAS
// ============================================================================

/**
 * @brief Lifecycle state of a circular buffer page slot `slots_[s]`.
 */
enum class PageSlotState : uint8_t {
    FREE_UNALLOCATED = 0,  // Slot is empty and ready to be claimed by Allocate()
    ACTIVE_MUTABLE   = 1,  // Page lies in [ReadOnlyAddress, TailAddress]; mutable in-place
    READ_ONLY_DIRTY  = 2,  // Page (or tail of page) is immutable and awaits WriteFixed flush
    FLUSH_IN_FLIGHT  = 3,  // PageFlusherFiber is currently inside io_.WriteFixed() on this slot
    FLUSHED_CLEAN    = 4   // Full page [0..P) is durable on NVMe; evictable when active_pins == 0
};

/**
 * @brief Per-slot descriptor for each of the $C = 2^k$ pages in the circular
 *        in-memory buffer. Occupies one 64-byte cache line.
 */
struct alignas(64) PageSlotDescriptor {
    tinyio::FixedBuffer dma_buf{};                                    // Registered P-byte huge-page DMA buffer
    uint64_t            logical_page_id       = UINT64_MAX;           // Logical page p(L) currently in this slot
    uint32_t            written_end_offset    = 0;                    // Highest intra-page byte offset staged (0..P)
    uint32_t            flushed_sector_offset = 0;                    // Highest 4KB-aligned offset durable on NVMe
    uint32_t            active_pins           = 0;                    // Count of live PageReadPin guards on this slot
    PageSlotState       state                 = PageSlotState::FREE_UNALLOCATED;

    [[nodiscard]] uint8_t* PagePtr() noexcept {
        return reinterpret_cast<uint8_t*>(dma_buf.Ptr());
    }
    [[nodiscard]] const uint8_t* PagePtr() const noexcept {
        return reinterpret_cast<const uint8_t*>(dma_buf.Ptr());
    }
};
static_assert(sizeof(PageSlotDescriptor) == 64, "PageSlotDescriptor must fit in one 64B cache line");

/**
 * @brief 4096-byte O_DIRECT Snapshot Manifest (`hybrid_log_meta_<shard>.snap`)
 *        for persisting and recovering the HybridLog watermarks.
 */
struct alignas(SECTOR_SIZE) HybridLogSnapshotMeta {
    uint64_t magic                 = HYBRIDLOG_MAGIC;
    uint32_t format_version        = HYBRIDLOG_FORMAT_VER;
    uint32_t shard_id              = 0;
    uint64_t snapshot_epoch        = 0;
    uint64_t begin_address         = kInitialLogicalAddress;
    uint64_t flushed_until_address = kInitialLogicalAddress;
    uint32_t page_size_shift       = kDefaultPageShift;
    uint32_t segment_pages_shift   = kDefaultSegmentShift;
    uint64_t header_checksum       = 0;  // CRC32C-based 64-bit checksum of bytes [0x00..0x2F]
    uint8_t  reserved[SECTOR_SIZE - 56]{};
};
static_assert(sizeof(HybridLogSnapshotMeta) == SECTOR_SIZE,
              "HybridLogSnapshotMeta must be 4096 bytes for O_DIRECT");

// ============================================================================
// SECTION 4: CONFIGURATION, INPUTS & OUTPUT VIEWS
// ============================================================================

struct HybridLogConfig {
    std::filesystem::path data_dir{"/var/tmp/diskbench/hybridlog"};
    uint32_t              page_size_shift         = kDefaultPageShift;     // m: P = 2^m bytes
    uint32_t              buffer_pages_shift      = kDefaultBufferShift;   // k: C = 2^k pages in RAM
    uint32_t              readonly_pipeline_pages = 8;                     // C_ro >= 2 flush pipeline pages
    uint32_t              segment_pages_shift     = kDefaultSegmentShift;  // s: S = 2^s pages per segment
    uint32_t              cold_read_scratch_slots = 2;                     // FixedBuffers for cold reads
    bool                  direct_io               = true;                  // O_DIRECT on segment files
};

/**
 * @brief Classification of a `LogicalAddress` across the HybridLog watermarks.
 */
enum class AddressRegion : uint8_t {
    TRUNCATED_EXPIRED = 0,  // L < BeginAddress (reclaimed by TruncateBefore)
    COLD_ON_DISK      = 1,  // BeginAddress <= L < HeadAddress (requires ReadColdRecord)
    WARM_READONLY     = 2,  // HeadAddress <= L < ReadOnlyAddress (immutable in-memory view)
    HOT_MUTABLE       = 3,  // ReadOnlyAddress <= L < TailAddress (mutable in-place view)
    OUT_OF_BOUNDS     = 4   // L >= TailAddress or invalid
};

/**
 * @brief Direct in-memory writable view of a record in `[ReadOnlyAddress, TailAddress)`.
 */
struct MutableRecordView {
    uint64_t              logical_address = kInvalidLogicalAddress;
    LogRecordHeader*      header          = nullptr;
    std::span<std::byte>  capacity_span{};  // Full writable `[0 .. header->payload_capacity)`

    [[nodiscard]] std::span<std::byte> ActivePayload() const noexcept {
        return capacity_span.first(header ? header->payload_size : 0);
    }
};

/**
 * @brief Direct in-memory read-only view of a record in `[HeadAddress, TailAddress)`.
 */
struct ReadOnlyRecordView {
    uint64_t                   logical_address = kInvalidLogicalAddress;
    const LogRecordHeader*     header          = nullptr;
    std::span<const std::byte> payload{};  // Immutable `[0 .. header->payload_size)`
};

/**
 * @brief Result returned by synchronous `HybridLog::ResolveAddress(L)`.
 */
struct AddressResolution {
    AddressRegion      region        = AddressRegion::OUT_OF_BOUNDS;
    MutableRecordView  mutable_view{};   // Populated iff region == HOT_MUTABLE
    ReadOnlyRecordView readonly_view{};  // Populated iff region == WARM_READONLY or HOT_MUTABLE
};

/**
 * @brief Result returned by asynchronous `HybridLog::ReadColdRecord(L, out_payload)`.
 */
struct ColdRecordOutcome {
    uint64_t        logical_address = kInvalidLogicalAddress;
    LogRecordHeader header{};
};

/**
 * @brief Callback invoked by `HybridLog::Scan()` for each valid `NORMAL_RECORD`.
 *        Return `true` to continue scanning, or `false` to stop early.
 */
using LogScanCallback = bool (*)(
    void*                      ctx,
    uint64_t                   logical_address,
    const LogRecordHeader&     header,
    std::span<const std::byte> payload
);

/**
 * @brief Stack-allocated wait node for `HybridLog::WaitForFlushUntil()`.
 *
 * One node per waiting fiber, linked into `flush_waiters_` ordered by
 * `target`. The flusher posts `done` exactly once, when
 * `flushed_until_address_ >= target`; the node dies with the waiter's stack
 * frame, which is safe because a posted Baton is never touched again.
 */
struct FlushWaiterNode {
    uint64_t target = 0;  // Watermark this waiter needs durable
    tinyio::Baton done;   // One-shot; posted once by the flusher
    FlushWaiterNode* next = nullptr;

    explicit FlushWaiterNode(tinyio::IO& io) noexcept : done(io) {}
};

// ============================================================================
// SECTION 5: `HybridLog` ENGINE SPECIFICATION (DECLARATION-ONLY INTERFACE)
// ============================================================================

class HybridLog {
public:
    /**
     * @brief RAII pin guard (Gap 7 Baseline) that prevents `TryEvictFlushedPages()`
     *        from advancing `SafeHeadAddress` past `slot_idx` while a fiber holds
     *        a zero-copy `ReadOnlyRecordView` across an I/O suspension point.
     */
    class PageReadPin {
    public:
        PageReadPin() noexcept = default;
        PageReadPin(const PageReadPin&)            = delete;
        PageReadPin& operator=(const PageReadPin&) = delete;

        PageReadPin(PageReadPin&& other) noexcept
            : log_(std::exchange(other.log_, nullptr)),
              slot_idx_(std::exchange(other.slot_idx_, 0)) {}

        PageReadPin& operator=(PageReadPin&& other) noexcept;
        ~PageReadPin() noexcept { Release(); }

        void Release() noexcept;
        [[nodiscard]] bool Valid() const noexcept { return log_ != nullptr; }

    private:
        friend class HybridLog;
        PageReadPin(HybridLog* log, size_t slot_idx) noexcept
            : log_(log), slot_idx_(slot_idx) {}

        HybridLog* log_      = nullptr;
        size_t     slot_idx_ = 0;
    };

private:
    struct SegmentFileEntry {
        uint64_t         segment_id = UINT64_MAX;
        tinyio::UniqueFd fd{};
    };

    // ------------------------------------------------------------------------
    // 1. Runtime & Derived Geometry Constants (Eq. 1 - Eq. 6)
    // ------------------------------------------------------------------------
    tinyio::IO&     io_;
    HybridLogConfig cfg_{};
    uint32_t        shard_id_           = 0;
    int             fatal_io_err_       = 0;

    size_t          page_size_          = 0;  // P = 1ULL << page_size_shift
    size_t          page_mask_          = 0;  // P - 1
    size_t          num_pages_          = 0;  // C = 1ULL << buffer_pages_shift
    size_t          slot_mask_          = 0;  // C - 1
    size_t          mutable_pages_      = 0;  // C_mutable = C - 1 - readonly_pipeline_pages
    size_t          pages_per_segment_  = 0;  // S = 1ULL << segment_pages_shift
    uint32_t        segment_byte_shift_ = 0;  // m + s
    uint64_t        segment_byte_mask_  = 0;  // (S * P) - 1

    // ------------------------------------------------------------------------
    // 2. The Seven Monotonic HybridLog Watermarks (Eq. 7 - Eq. 10)
    // ------------------------------------------------------------------------
    uint64_t begin_address_          = kInitialLogicalAddress;
    uint64_t safe_head_address_      = kInitialLogicalAddress;
    uint64_t head_address_           = kInitialLogicalAddress;
    uint64_t flushed_until_address_  = kInitialLogicalAddress;
    uint64_t safe_read_only_address_ = kInitialLogicalAddress;
    uint64_t read_only_address_      = kInitialLogicalAddress;
    uint64_t tail_address_           = kInitialLogicalAddress;

    // ------------------------------------------------------------------------
    // 3. Registered DMA Circular Buffer Slots, Scratch Pool & Segment FDs
    // ------------------------------------------------------------------------
    std::vector<PageSlotDescriptor>  slots_{};                 // Size C = 2^k registered page slots
    std::vector<tinyio::FixedBuffer> cold_scratch_bufs_{};     // Scratch FixedBuffers for Cold Reads
    std::vector<uint32_t>            free_scratch_indices_{};  // Free scratch buffer ordinals
    std::optional<tinyio::Semaphore> scratch_slots_{};         // FIFO counting semaphore for cold-read scratch leases
    std::vector<SegmentFileEntry>    segments_{};              // Open pre-allocated O_DIRECT segments

    // ------------------------------------------------------------------------
    // 4. Background Page Flusher Fiber & Backpressure Primitives
    // ------------------------------------------------------------------------
    // The flusher sleeps on a fresh one-shot Baton published through
    // `flusher_wake_` (contract in tinyio/runtime/fiber_sync.hpp):
    //   - re-check the predicate BEFORE creating and publishing the baton;
    //   - the poster clears the pointer (`std::exchange`) BEFORE `Post()`,
    //     so it never dangles at the waiter's resumed stack frame;
    //   - nobody touches the baton after `Unpark()` — the waiter runs only
    //     when the poster yields.
    // A WakeFlusher() that lands while the flusher is inside io_.WriteFixed()
    // finds `flusher_wake_ == nullptr` and is dropped: not a lost wakeup,
    // because the writer advanced safe_read_only_address_ (the sticky state)
    // before waking. Shutdown() = set shutting_down_, then WakeFlusher();
    // the loop condition ends the fiber with nobody left parked.
    tinyio::Baton* flusher_wake_ = nullptr;  // Sleeping flusher's baton; null while busy
    bool shutting_down_ = false;
    // One credit per evictable circular slot: TryEvictFlushedPages()
    // Release()s when a slot resets to FREE_UNALLOCATED, Allocate() Acquire()s
    // before claiming the next tail page (Eq. 8 backpressure, FIFO-fair).
    std::optional<tinyio::Semaphore> eviction_slots_{};  // Constructed in Start()
    // WaitForFlushUntil() waiters, ordered by target (see FlushWaiterNode).
    FlushWaiterNode* flush_waiters_ = nullptr;

public:
    explicit HybridLog(tinyio::IO& io, HybridLogConfig cfg = {}) noexcept;
    ~HybridLog() noexcept;

    HybridLog(const HybridLog&)            = delete;
    HybridLog& operator=(const HybridLog&) = delete;
    HybridLog(HybridLog&&)                 = delete;
    HybridLog& operator=(HybridLog&&)      = delete;

    // ------------------------------------------------------------------------
    // O(1) Coordinate Mapping Equations (Eq. 1 - Eq. 6)
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr uint64_t LogicalPage(uint64_t addr) const noexcept {
        return addr >> cfg_.page_size_shift;
    }
    [[nodiscard]] constexpr size_t PageOffset(uint64_t addr) const noexcept {
        return static_cast<size_t>(addr & page_mask_);
    }
    [[nodiscard]] constexpr size_t SlotIndex(uint64_t addr) const noexcept {
        return static_cast<size_t>((addr >> cfg_.page_size_shift) & slot_mask_);
    }
    [[nodiscard]] constexpr uint64_t SegmentId(uint64_t addr) const noexcept {
        return addr >> segment_byte_shift_;
    }
    [[nodiscard]] constexpr off_t SegmentByteOffset(uint64_t addr) const noexcept {
        return static_cast<off_t>(addr & segment_byte_mask_);
    }

    // ------------------------------------------------------------------------
    // Watermark & Geometry Accessors
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr uint64_t BeginAddress() const noexcept { return begin_address_; }
    [[nodiscard]] constexpr uint64_t SafeHeadAddress() const noexcept { return safe_head_address_; }
    [[nodiscard]] constexpr uint64_t HeadAddress() const noexcept { return head_address_; }
    [[nodiscard]] constexpr uint64_t FlushedUntilAddress() const noexcept { return flushed_until_address_; }
    [[nodiscard]] constexpr uint64_t SafeReadOnlyAddress() const noexcept { return safe_read_only_address_; }
    [[nodiscard]] constexpr uint64_t ReadOnlyAddress() const noexcept { return read_only_address_; }
    [[nodiscard]] constexpr uint64_t TailAddress() const noexcept { return tail_address_; }
    [[nodiscard]] constexpr size_t   PageSize() const noexcept { return page_size_; }
    [[nodiscard]] constexpr size_t   CircularSlotCount() const noexcept { return num_pages_; }

    [[nodiscard]] constexpr AddressRegion ClassifyAddress(uint64_t addr) const noexcept {
        if (addr < kInitialLogicalAddress || addr >= tail_address_) {
            return AddressRegion::OUT_OF_BOUNDS;
        }
        if (addr < begin_address_) {
            return AddressRegion::TRUNCATED_EXPIRED;
        }
        if (addr < head_address_) {
            return AddressRegion::COLD_ON_DISK;
        }
        if (addr < read_only_address_) {
            return AddressRegion::WARM_READONLY;
        }
        return AddressRegion::HOT_MUTABLE;
    }

    // ------------------------------------------------------------------------
    // Lifecycle, Recovery & Shutdown API
    // ------------------------------------------------------------------------

    /**
     * @brief Lease `C + cold_read_scratch_slots` registered `FixedBuffer`s
     *        from `io_.TakeFixedBuffer()`, initialize Logical Page 0 at byte
     *        offset 64, and spawn `PageFlusherFiberLoop()`.
     */
    [[nodiscard]] tinyio::Result<void> Start() noexcept;

    /**
     * @brief Recover the HybridLog from a committed `HybridLogSnapshotMeta`:
     *        preload the warm tail pages `[warm_head, meta.flushed_until_address)`
     *        from NVMe segment files into `slots_[]` via `io_.ReadFixed()` and
     *        position `tail_address_ = read_only_address_ = meta.flushed_until_address`.
     */
    [[nodiscard]] tinyio::Result<void> RecoverFromSnapshot(
        const HybridLogSnapshotMeta& meta
    ) noexcept;

    /**
     * @brief Stop `PageFlusherFiberLoop()`, close open segment FDs via
     *        `io_.Close()`, and release all `FixedBuffer` leases before `~IO()`.
     */
    [[nodiscard]] tinyio::Result<void> Shutdown() noexcept;

    // ------------------------------------------------------------------------
    // Core HybridLog Operations: Allocate, Resolve, Mutate, Pin & Cold Read
    // ------------------------------------------------------------------------

    /**
     * @brief Execute Algorithm 1: Allocate a new record frame with payload
     *        capacity `AlignUp8(payload_size)` at `tail_address_`.
     *
     * If the record overflows the current page `p`, seals page `p` with a
     * `PAGE_END_SENTINEL`, advances `read_only_address_` (Eq. 9), and parks
     * on `eviction_slots_->Acquire()` if circular slot `slot(p + 1)` is not yet free.
     */
    [[nodiscard]] tinyio::Result<MutableRecordView> Allocate(
        uint32_t payload_size,
        uint16_t epoch_or_ver = 0,
        uint8_t  user_flags   = 0
    ) noexcept;

    /**
     * @brief Finalize the payload and hardware CRC32C of a newly allocated or
     *        in-place-mutated record inside the Mutable region
     *        (`view.logical_address >= read_only_address_`).
     *
     * Requires `new_payload_size <= view.header->payload_capacity`.
     */
    void CommitInPlaceMutation(
        MutableRecordView& view,
        uint32_t           new_payload_size,
        uint8_t            new_user_flags   = 0,
        uint16_t           new_epoch_or_ver = 0
    ) noexcept;

    /**
     * @brief Mark a record inside the Mutable region (`addr >= read_only_address_`)
     *        as `LogRecordKind::INVALID_SKIP` (e.g., when a variable-length
     *        update exceeds `payload_capacity` and is re-allocated at the tail).
     */
    void MarkMutableRecordInvalid(MutableRecordView& view) noexcept;

    /**
     * @brief Execute Algorithm 2: Resolve `addr` in O(1) without I/O.
     *        - Returns `HOT_MUTABLE` + `mutable_view` if `addr >= read_only_address_`.
     *        - Returns `WARM_READONLY` + `readonly_view` if `head_address_ <= addr < read_only_address_`.
     *        - Returns `COLD_ON_DISK` if `begin_address_ <= addr < head_address_`.
     */
    [[nodiscard]] AddressResolution ResolveAddress(uint64_t addr) noexcept;

    /**
     * @brief Acquire an RAII `PageReadPin` on an in-memory address
     *        `addr >= head_address_` (Gap 7 Baseline) so its circular slot
     *        cannot be evicted while the calling fiber suspends on other I/O.
     */
    [[nodiscard]] PageReadPin PinPage(uint64_t addr) noexcept;

    /**
     * @brief Execute Algorithm 3: Read and CRC-verify a cold record at
     *        `addr in [begin_address_, head_address_)` from its NVMe segment
     *        file using 4096B-aligned `io_.ReadFixed()`.
     */
    [[nodiscard]] tinyio::Result<ColdRecordOutcome> ReadColdRecord(
        uint64_t                addr,
        std::vector<std::byte>& out_payload
    ) noexcept;

    // ------------------------------------------------------------------------
    // Explicit Watermark Transitions, Flushing, Scanning & Truncation
    // ------------------------------------------------------------------------

    /**
     * @brief Fold `read_only_address_` and `safe_read_only_address_` all the
     *        way to `tail_address_` (making all currently allocated records
     *        immutable and waking `PageFlusherFiberLoop()`).
     *
     * @return The captured `tail_address_` watermark to pass to `WaitForFlushUntil()`.
     */
    [[nodiscard]] uint64_t ShiftReadOnlyToTail() noexcept;

    /**
     * @brief Park the calling fiber until `flushed_until_address_ >= target_addr`
     *        by linking a stack node `{target, one-shot Baton, next}` into
     *        `flush_waiters_`; the flusher posts the node's Baton exactly
     *        once. One wait, one baton — reuse would only add risk.
     */
    [[nodiscard]] tinyio::Result<void> WaitForFlushUntil(uint64_t target_addr) noexcept;

    /**
     * @brief Persist a 4096B `HybridLogSnapshotMeta` manifest capturing
     *        `[begin_address_, flushed_until_address_]` after folding/flushing.
     */
    [[nodiscard]] tinyio::Result<HybridLogSnapshotMeta> WriteSnapshotManifest(
        uint64_t snapshot_epoch
    ) noexcept;

    /**
     * @brief Execute Algorithm 6: Sequentially scan all valid `NORMAL_RECORD`
     *        frames across `[from_addr, to_addr)` (seamlessly spanning Cold
     *        NVMe pages and Warm/Hot in-memory circular slots).
     */
    [[nodiscard]] tinyio::Result<void> Scan(
        uint64_t        from_addr,
        uint64_t        to_addr,
        LogScanCallback callback,
        void*           callback_ctx
    ) noexcept;

    /**
     * @brief Advance `begin_address_` to `new_begin_addr` and unlink any
     *        on-disk segment files strictly below `SegmentId(new_begin_addr)`.
     */
    [[nodiscard]] tinyio::Result<void> TruncateBefore(uint64_t new_begin_addr) noexcept;

private:
    // ------------------------------------------------------------------------
    // Internal Page Rotation, Flushing & Segment File Helpers
    // ------------------------------------------------------------------------
    void SealPageTail(uint64_t page_id, size_t cur_off) noexcept;
    void AdvanceReadOnlyWatermarkTo(uint64_t target_ro) noexcept;
    void UnpinSlot(size_t slot_idx) noexcept;

    void PageFlusherFiberLoop() noexcept;
    void TryEvictFlushedPages() noexcept;
    void WakeFlusher() noexcept;

    [[nodiscard]] tinyio::Result<const tinyio::UniqueFd*> GetOrOpenSegmentFd(uint64_t seg_id) noexcept;
};

}  // namespace storage::hybridlog
