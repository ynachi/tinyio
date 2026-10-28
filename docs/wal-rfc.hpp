/**
 * ============================================================================
 * RFC-WAL-2026 (SPLIT ARCHITECTURE): WIRE FORMAT & WAL WRITER SPECIFICATION
 * ============================================================================
 *
 * Status:       Production Split Writer/Reader Specification (for tinyio::IO)
 * Target:       C++23, tinyio::IO (io_uring WRITE_FIXED / READ_FIXED, O_DIRECT | O_DSYNC)
 * Checksum:     google/crc32c (SSE4.2+PCLMULQDQ / ARMv8 CRC32C+PMULL runtime dispatch)
 * Concurrency:  Single-Threaded Shared-Nothing Stackful Fibers (Thread-per-Core)
 * Alignment:    4096-byte Physical Sector LBA Format (O_DIRECT, 4KB Sub-Sector Safe)
 *
 * ============================================================================
 * 1. ARCHITECTURAL OVERVIEW: WHY SPLIT `WalWriter` AND `WalReader`?
 * ============================================================================
 * In a shared-nothing, thread-per-core storage engine built on `tinyio::IO`,
 * the active write path and the recovery/replay path have fundamentally
 * asymmetric resource lifetimes:
 *
 * +-------------------------------------+-------------------------------------+
 * |       WalWriter (Active Head)       |    WalReader (Recovery / Replay)    |
 * +-------------------------------------+-------------------------------------+
 * | - Singleton per CPU shard           | - Ephemeral or background scanner   |
 * | - Leases 2 FixedBuffers (2 x 256KB) | - Leases ONLY 1 FixedBuffer (256KB) |
 * | - Spawns 1 background Flusher Fiber | - Spawns 0 background fibers        |
 * | - Mutates sub-sector tail in-place  | - Read-only; produces TailState     |
 * +-------------------------------------+-------------------------------------+
 *
 *   1. Multi-Segment Crash Recovery:
 *      Suppose a shard crashes with 4 uncheckpointed WAL segments `[40, 41, 42, 43]`.
 *      Segments `40`, `41`, and `42` are sealed historical files; only Segment `43`
 *      was actively being written at power loss. We use a single `WalReader`
 *      (holding 1 `FixedBuffer`) to recover and replay `40..43`, and only at the
 *      very end of Segment `43` do we hand its `RecoveredTailState` to `WalWriter`!
 *
 *   2. Concurrent Replication / CDC (Change Data Capture):
 *      While `WalWriter` is appending to Segment `43` at 1M+ IOPS, a background
 *      fiber on the same core can instantiate a `WalReader` to stream committed
 *      records from Segment `42` to a Raft follower or compaction job without
 *      interfering with `WalWriter`'s ping-pong DMA buffers.
 *
 *   3. Zero-Read Tail Handoff (`RecoveredTailState`):
 *      When `WalReader::RecoverSegmentTail()` finishes inspecting the active
 *      head segment on boot, it has *already* read the final valid 4096-byte
 *      sector from NVMe into memory (and stripped any trailing uncommitted
 *      `FIRST` fragment from a torn multi-block write). It snapshots that 4KB
 *      sector inside `RecoveredTailState::tail_sector_data`. When
 *      `WalWriter::ResumeFromTail()` is called, it copies the valid byte prefix
 *      of that sector directly into Block 0 of `dma_buffers_[0]` so the very
 *      first post-crash write appends in-place inside that partial 4KB sector
 *      with ZERO extra NVMe reads!
 *
 * ============================================================================
 * 2. PHYSICAL ON-DISK SEGMENT, IMMUTABLE BLOCK HEADER & PER-RECORD CRC
 * ============================================================================
 * Every WAL segment file is pre-allocated to a fixed size (default 256 MiB =
 * 65,536 blocks of 4096 bytes) via `tinyio::IO::Fallocate()` so runtime writes
 * NEVER extend the file size or acquire VFS inode locks (`i_rwsem`).
 *
 * A. 256 MiB Pre-Allocated Segment File Layout (65,536 x 4KB Blocks):
 * +------------------------+------------------------+------------------------+
 * |        Block 0         |     Blocks 1 .. K      |  Blocks K+1 .. 65,535  |
 * | WalSegmentHeader (4KB) | Active Data (Epoch E)  | Stale Data (Epoch E-1) |
 * +------------------------+------------------------+------------------------+
 *
 * B. Single 4096-Byte Physical NVMe Data Sector (Blocks 1 .. 65,535):
 * +--------------------------------------------------------------------------+
 * |                     4096-Byte Physical NVMe Sector                       |
 * +--------------------+--------------------+--------------------+-----------+
 * |  WalBlockHeader    |  Record Fragment 1 |  Record Fragment 2 | Zero Pad  |
 * | (16B, IMMUTABLE!)  | (16B Hdr+CRC + Pay)| (16B Hdr+CRC + Pay)| (0..4063B)|
 * +--------------------+--------------------+--------------------+-----------+
 * ^                    ^                                         ^           ^
 * Byte 0               Byte 16                    cur_block_offset_  Byte 4096
 *
 * C. Why `WalBlockHeader` Is Immutable & Checksums Live in `WalRecordHeader`:
 *    When a partially filled 4KB tail block is rewritten in-place across
 *    multiple `WriteFixed` flushes:
 *      - If a whole-block checksum lived in `WalBlockHeader`, rewriting the
 *        4KB block to append Record 2 at offset 600 would mutate bytes `[0..15]`
 *        (in the first 512B sub-sector). If power failed on a 512e drive (or
 *        simulator) after `[0..511]` landed but before `[512..1535]` landed,
 *        the whole-block checksum would fail and destroy Record 1 (`[16..599]`),
 *        which was ALREADY acknowledged as durable!
 *      - By making `WalBlockHeader` (`[0..15]`) 100% IMMUTABLE once the first
 *        fragment is written at offset 16, and placing a hardware `record_crc`
 *        (seeded with `segment_epoch`) inside each 16-byte `WalRecordHeader`:
 *          1. Bytes `[0 .. cur_block_offset_)` are NEVER modified once staged.
 *          2. An in-place 4KB tail rewrite writes the exact same bytes over
 *             `[0 .. cur_block_offset_)`, so even a 512B sub-sector tear in
 *             `[cur_block_offset_ .. 4095]` can only invalidate the new,
 *             unacknowledged record — NEVER an already-acknowledged record!
 *          3. Worker fibers checksum only their own fragment (`16 + payload_len`
 *             bytes) once during `StageRecord()`; `FlusherFiberLoop()` performs
 *             ZERO payload CRC work on the flush critical path.
 *
 * ============================================================================
 * 3. WHY `WalWriter` LEASES TWO `tinyio::FixedBuffer` SLOTS (PING-PONG DMA)
 * ============================================================================
 * Why does a single-threaded `WalWriter` need TWO registered DMA buffers
 * (`dma_buffers_[0]` and `dma_buffers_[1]`)?
 *
 *   1. Hardware DMA Concurrency & Memory Safety:
 *      When `FlusherFiberLoop()` calls `io_.WriteFixed(fd_, dma_buffers_[0], ...)`,
 *      the NVMe controller's DMA engine reads directly from `dma_buffers_[0]`'s
 *      physical RAM pages over PCIe for ~10-25 microseconds. If a running CPU
 *      fiber wrote a new record into `dma_buffers_[0]` while the DMA transfer
 *      was in flight, it would corrupt the bytes mid-transfer.
 *
 *   2. Pipelining CPU Staging with NVMe I/O:
 *      By swapping `active_buf_idx_ ^= 1` and waking overflow waiters *before*
 *      calling `io_.WriteFixed()`, worker fibers on the CPU immediately stage
 *      new records into `dma_buffers_[1]` at L1/L2 cache speed while
 *      `dma_buffers_[0]` is in flight to the SSD:
 *
 * +-------------------------------------+-------------------------------------+
 * |     dma_buffers_[0] (In-Flight)     |     dma_buffers_[1] (CPU Active)    |
 * +-------------------------------------+-------------------------------------+
 * | Locked by FlusherFiberLoop inside   | Open for Worker Fibers calling      |
 * | io_.WriteFixed() -> NVMe PCIe DMA   | StageRecord() at L1/L2 cache speed  |
 * +-------------------------------------+-------------------------------------+
 *
 *   3. Sub-Sector Tail Carryover Across Buffer Swaps:
 *      What if `dma_buffers_[0]` flushes when its last 4KB sector (say Block 2)
 *      only has 584 bytes of payload (`cur_block_offset_ == 600`)?
 *      Before issuing `WriteFixed`, `SwapAndPrepareFlush()` zeroes `[600..4095]`
 *      in `dma_buffers_[0]` and copies the immutable prefix `[0..599]` into
 *      Block 0 of `dma_buffers_[1]`:
 *
 *   dma_buffers_[0] (Flushing 3 blocks: Disk Sectors D, D+1, D+2):
 *   +-------------------+-------------------+-------------------+------------+
 *   |  Blk 0 (Sector D) | Blk 1 (Sector D+1)| Blk 2 (Sector D+2)|            |
 *   |   Full (4096 B)   |   Full (4096 B)   | [0..599] + 0-Pad  |            |
 *   +-------------------+-------------------+-------------------+------------+
 *                                                |
 *                    memcpy(600 bytes) ----------+
 *                                                v
 *   dma_buffers_[1] (New Active Buffer, anchored at Disk Sector D+2!):
 *   +---------------------------------------+--------------------------------+
 *   |           Blk 0 (Sector D+2)          |         Blk 1 .. Blk 63        |
 *   | [0..599 Copied] | <-- Next Fiber @600 |         Empty Staging          |
 *   +---------------------------------------+--------------------------------+
 *
 *      When `dma_buffers_[1]` later flushes, its Block 0 overwrites Disk
 *      Sector `D+2`, leaving `[0..599]` byte-for-byte unchanged and appending
 *      `[600..]` with zero wasted disk space!
 *
 *      Optimization — No Carryover of Exhausted Sectors:
 *      If `StageRecord()` finishes a record at `cur_block_offset_ > 4096 - MIN_REC_SPACE`
 *      (fewer than 17 bytes left in the sector, so no future record can start
 *      in it), `StageRecord()` immediately advances to the next block (when
 *      `cur_buf_block_ + 1 < blocks_per_dma_buf_`) so `SwapAndPrepareFlush()`
 *      never carries over or re-writes an already-exhausted sector.
 *
 * ============================================================================
 * 4. FIBER CONCURRENCY, OVERFLOW ADMISSION & CRASH-RECOVERY INVARIANTS
 * ============================================================================
 *   Invariant 1 (Zero-Allocation Group Commit via Intrusive Stack `tinyio::Baton` Nodes):
 *      `WalWriter` pairs two singly-linked lists of stack nodes `SyncWaiterNode* sync_waiters_[2]`
 *      with `dma_buffers_[2]`:
 *        - A fiber that finishes staging `my_lsn` into `dma_buffers_[active_buf_idx_]`
 *          links a stack-allocated `SyncWaiterNode { Baton done; ... }` into
 *          `sync_waiters_[active_buf_idx_]` and calls `node.done.Wait()`.
 *        - Flipping `active_buf_idx_ ^= 1` in `SwapAndPrepareFlush()`
 *          automatically isolates the in-flight batch (`sync_waiters_[flush_idx]`)
 *          from incoming fibers staging into the new active buffer.
 *        - When `WriteFixed()` completes, `FlusherFiberLoop()` updates
 *          `durable_lsn_ = flush_max_lsn` (or latches `fatal_io_err_`) and
 *          posts the `Baton` of every node in `sync_waiters_[flush_idx]`.
 *
 *   Invariant 2 (Strict FIFO Admission & Atomic Multi-Block Staging):
 *      A record > 4064 bytes fragments into `FIRST` -> `MIDDLE`* -> `LAST`.
 *      If a record > 256 KiB overflows the active `FixedBuffer`, `StageRecord()`
 *      suspends inside `EnsureBufferSpace()` until `FlusherFiberLoop()` swaps
 *      the buffers. While `staging_busy_` is true or
 *      `space_overflow_head_ != nullptr`, newly arriving fibers in
 *      `AppendAndSync()` MUST park on `SpaceWaiterNode` BEFORE
 *      acquiring `next_lsn_++`. This guarantees:
 *        (a) No fiber can interleave a `FULL` record between another fiber's
 *            `FIRST`/`MIDDLE`/`LAST` fragments across a buffer swap.
 *        (b) `lsn` is assigned strictly in on-disk byte order, preserving
 *            monotonic block LSNs across the entire 256 MiB segment.
 *
 *   Invariant 3 (Flushing Mid-Record Overflow When `sync_waiters_` Is Empty):
 *      When a >256 KiB record fills all 64 blocks of `dma_buffers_[0]` with
 *      `FIRST`/`MIDDLE` fragments, the owning fiber parks in `space_overflow_head_`
 *      (it has not parked on `sync_waiters_` yet because its `LAST` fragment
 *      has not been staged). Therefore, `FlusherFiberLoop()` MUST trigger
 *      `WriteFixed()` whenever `out_bytes_to_write > 0`, even when
 *      `sync_waiters_[flush_idx] == nullptr`, and `durable_lsn_` only
 *      advances to `staged_complete_lsn_` (the highest LSN whose `FULL` or
 *      `LAST` fragment is completely staged in the flushed buffer).
 *
 *   Invariant 4 (Latched I/O Error Prevents Unwritten Disk Holes):
 *      Because `SwapAndPrepareFlush()` advances `buf_start_disk_block_` and
 *      copies the partial tail into the next buffer *before* `WriteFixed()`
 *      completes, any `WriteFixed()` failure latches `fatal_io_err_`. Once
 *      latched, `WalWriter` refuses to flush subsequent batches (which would
 *      write Sector `D+2` past unwritten Sectors `D..D+1`) and fails all
 *      current and future callers with `fatal_io_err_`.
 *
 *   Invariant 5 (Defeating Out-of-Order LBA Tears & Phantom Blocks on Recovery):
 *      If power is lost while a 64-block `WriteFixed()` is in flight, NVMe
 *      may commit Block `12` (`epoch = E`) to flash before Block `11`
 *      (`epoch = E-1`). `WalReader` stops at Block `11`, and
 *      `WalWriter::ResumeFromTail()` both enforces strict LSN continuity
 *      (`prev_lsn` -> `prev_lsn + 1`) and zeroes the 64-block DMA tear window
 *      past `last_valid_block_idx` once at boot (~20 us) so phantom Block `12`
 *      can never resurrect after a second crash.
 * ============================================================================
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

// `linux/fs.h` (pulled in transitively by <liburing.h> above) defines a
// `BLOCK_SIZE` macro. The guard must sit AFTER the includes that define it —
// an #undef placed above them is dead, because the macro comes back with the
// next include, and `BLOCK_SIZE = 4096` below fails to parse.
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif

namespace storage::wal::split {

// ============================================================================
// SECTION 1: CONSTANTS & WIRE FORMAT GEOMETRY
// ============================================================================

inline constexpr uint64_t WAL_MAGIC          = 0x57414C5F32303236ULL;  // ASCII "WAL_2026"
inline constexpr uint32_t WAL_FORMAT_VER     = 1;

inline constexpr size_t   BLOCK_SIZE         = 4096;
inline constexpr size_t   BLOCK_HDR_SIZE     = 16;
inline constexpr size_t   REC_HDR_SIZE       = 16;
inline constexpr size_t   MAX_BLOCK_PAYLOAD  = BLOCK_SIZE - BLOCK_HDR_SIZE;       // 4080 B
inline constexpr size_t   MAX_FRAG_PAYLOAD   = MAX_BLOCK_PAYLOAD - REC_HDR_SIZE;  // 4064 B
inline constexpr size_t   MIN_REC_SPACE      = REC_HDR_SIZE + 1;                  // 17 B

// Default FixedBuffer slot size expected from `tinyio::IoOptions::buffer_pool`:
// 64 blocks * 4096B = 256 KiB per FixedBuffer lease (divides 1 GiB evenly).
inline constexpr size_t   DEFAULT_DMA_BLOCKS = 64;
inline constexpr size_t   DEFAULT_DMA_BYTES  = DEFAULT_DMA_BLOCKS * BLOCK_SIZE;

// Default segment file size: 256 MiB = 65,536 blocks of 4KB.
inline constexpr uint64_t DEFAULT_SEG_BLOCKS = 65536;

// Sentinel value for WalBlockHeader::first_rec_offset when a 4KB block has no
// starting FULL or FIRST record (e.g., 100% consumed by MIDDLE or LAST).
inline constexpr uint16_t NO_NEW_RECORD      = 0xFFFF;

// ============================================================================
// SECTION 2: ON-DISK WIRE FORMAT STRUCTURES (4KB SECTOR ALIGNED)
// ============================================================================

/**
 * @brief Block 0 of every WAL Segment file (occupies one full 4096B sector).
 *
 * Horizontal Byte Framing (64-Byte Active Header + 4032-Byte Zero Padding):
 * +--------------------------------------------------------------------------+
 * |                  WalSegmentHeader (4096-Byte Block 0)                    |
 * +--------------+--------------+--------------+--------------+--------------+
 * | magic (8B)   | version (4B) | shard_id(4B) | seg_id (8B)  | epoch (8B)   |
 * | [0x00..0x07] | [0x08..0x0B] | [0x0C..0x0F] | [0x10..0x17] | [0x18..0x1F] |
 * +--------------+--------------+--------------+--------------+--------------+
 * | base_lsn(8B) | prev_max(8B) | seg_blks(8B) | hdr_crc (8B) | reserved     |
 * | [0x20..0x27] | [0x28..0x2F] | [0x30..0x37] | [0x38..0x3F] | [64..4095]   |
 * +--------------+--------------+--------------+--------------+--------------+
 *
 * Durability Ordering Rule:
 *   When formatting or recycling a segment in `InitOrRecycleSegment()`, Block 0
 *   MUST be written and flushed via `io_.Fsync(fd_, true)` BEFORE any data
 *   block (Blocks 1..N-1) with `new_epoch` is written. `static_cast<uint32_t>(segment_epoch)`
 *   MUST be non-zero so valid blocks never collide with zero-initialized blocks.
 */
struct alignas(BLOCK_SIZE) WalSegmentHeader {
    uint64_t magic;                      // [0x00..0x07] Must equal WAL_MAGIC
    uint32_t version;                    // [0x08..0x0B] Must equal WAL_FORMAT_VER
    uint32_t shard_id;                   // [0x0C..0x0F] Owning tinyio::IO::Id()
    uint64_t segment_id;                 // [0x10..0x17] Logical monotonically increasing segment number
    uint64_t segment_epoch;              // [0x18..0x1F] Physical reuse counter (low 32 bits != 0)
    uint64_t base_lsn;                   // [0x20..0x27] First LSN expected at Block 1, Offset 16
    uint64_t prev_segment_max_lsn;       // [0x28..0x2F] Highest LSN committed in segment_id - 1 (0 if none)
    uint64_t segment_size_blocks;        // [0x30..0x37] Total 4KB blocks in file (including Block 0)
    uint64_t header_checksum;            // [0x38..0x3F] ComputeWalChecksum64 of bytes [0x00..0x37]
    uint8_t  reserved[BLOCK_SIZE - 64];  // Zero-filled padding to 4096 bytes
};
static_assert(sizeof(WalSegmentHeader) == BLOCK_SIZE, "WalSegmentHeader must be 4096 bytes");

/**
 * @brief Immutable 16-byte header placed at byte offset 0 of EVERY data block
 *        (Blocks 1 .. N-1).
 *
 * Horizontal Byte Framing (16 Bytes — Written ONCE when the block opens!):
 * +--------------------------------------------------------------------------+
 * |                 WalBlockHeader (16 Bytes, IMMUTABLE)                     |
 * +------------------------+--------------------+--------------+-------------+
 * | block_header_crc (8B)  | segment_epoch (4B) | 1st_off (2B) | reserved(2B)|
 * |     [0x00 .. 0x07]     |   [0x08 .. 0x0B]   | [0x0C..0x0D] | [0x0E..0x0F]|
 * +------------------------+--------------------+--------------+-------------+
 *                          |<--- Covered by block_header_crc ->|
 *
 * Why `WalBlockHeader` Never Mutates Across Sub-Sector Tail Rewrites:
 *   - `block_header_crc`: Checksum64 of bytes `[0x08..0x0F]` only.
 *   - `segment_epoch`   : Lower 32 bits of `WalSegmentHeader::segment_epoch` (`!= 0`).
 *   - `first_rec_offset`: Determined completely when the block's first fragment
 *                         is placed at offset 16:
 *                           * `16` if the fragment at offset 16 is `FULL` or `FIRST`;
 *                           * `NO_NEW_RECORD` (`0xFFFF`) if the fragment at offset 16
 *                             is `MIDDLE` (or `LAST` leaving `< MIN_REC_SPACE` bytes);
 *                           * `32 + last_frag_payload_len` if the fragment at offset 16
 *                             is `LAST` and leaves `>= MIN_REC_SPACE` bytes in the block.
 */
struct alignas(8) WalBlockHeader {
    uint64_t block_header_crc;  // [0x00..0x07] ComputeWalChecksum64 of bytes [0x08..0x0F]
    uint32_t segment_epoch;  // [0x08..0x0B] Lower 32 bits of WalSegmentHeader::segment_epoch (!= 0)
    uint16_t first_rec_offset;  // [0x0C..0x0D] Offset (16..4079) of first FULL/FIRST record, or
                                // NO_NEW_RECORD
    uint16_t reserved;          // [0x0E..0x0F] Must be 0
};
static_assert(sizeof(WalBlockHeader) == BLOCK_HDR_SIZE, "WalBlockHeader must be 16 bytes");

/**
 * @brief Record fragmentation tag (RocksDB-style state machine).
 *
 * How a Large Record (e.g., 10 KiB) Spans Across Three 4KB Sectors:
 * +------------------------+------------------------+------------------------+
 * |        Block 10        |        Block 11        |        Block 12        |
 * |  1st_off = 16          |  1st_off = 0xFFFF      |  1st_off = 1896        |
 * +------------------------+------------------------+------------------------+
 * | [FIRST (lsn=99, 4064B)]| [MIDDLE(lsn=99, 4064B)]| [LAST (lsn=99, 1864B)] |
 * |                        |                        | [FULL (lsn=100, 500B)] |
 * +------------------------+------------------------+------------------------+
 */
enum class RecordType : uint8_t {
    PADDING = 0,  // Zeroed tail space at end of block
    FULL    = 1,  // Complete record fits entirely within this single 4KB block
    FIRST   = 2,  // First chunk of a record that spills into subsequent block(s)
    MIDDLE  = 3,  // Intermediate chunk consuming an entire 4064B block payload
    LAST    = 4   // Final chunk of a multi-block fragmented record
};

/**
 * @brief Bitmask flags for WalRecordHeader::flags.
 */
namespace RecordFlags {
inline constexpr uint8_t NONE       = 0x00;
inline constexpr uint8_t COMPRESSED = 0x01;  // Payload is LZ4/Zstd compressed
inline constexpr uint8_t TX_BEGIN   = 0x02;  // Multi-record transaction start
inline constexpr uint8_t TX_COMMIT  = 0x04;  // Multi-record transaction commit marker
inline constexpr uint8_t TX_ABORT   = 0x08;  // Multi-record transaction rollback marker
inline constexpr uint8_t CHECKPOINT = 0x10;  // Fuzzy checkpoint watermark record
}  // namespace RecordFlags

/**
 * @brief Self-checksummed 16-byte prefix before every record fragment inside
 *        the 4080B block payload area.
 *
 * Horizontal Byte Framing (16 Bytes + Variable Fragment Payload):
 * +--------------------------------------------------------------------------+
 * |            WalRecordHeader (16 Bytes) + Fragment Payload                 |
 * +-------------------+----------------+---------------+--------+--------+---+
 * |     lsn (8B)      | record_crc(4B) |len_and_type(2)|flags(1)|meta(1B)|Pay|
 * |   [0x00 .. 0x07]  |  [0x08..0x0B]  | [0x0C..0x0D]  | [0x0E] | [0x0F] |...|
 * +-------------------+----------------+---------------+--------+--------+---+
 * |<- CRC32C covers: epoch_lo32 + lsn + [0x0C..0x0F] + Payload (0..4064B) -->|
 *
 * Critical Invariants:
 *   1. Per-Fragment Self-Checksumming (`record_crc`):
 *      Covers `(segment_epoch_lo32, lsn, len_and_type, flags, user_meta, payload)`.
 *      Guaranteed non-zero (`0` is reserved for zeroed tail padding).
 *      Because each fragment carries its own CRC and `segment_epoch` seed,
 *      in-place sub-sector appends NEVER modify earlier bytes in the 4KB block.
 *   2. Checkpoint LSN Binary Search:
 *      `lsn` is stamped on EVERY fragment (`FULL`, `FIRST`, `MIDDLE`, `LAST`).
 *      Reading the 8-byte `lsn` at byte offset 16 of ANY active 4KB block
 *      yields a monotonically non-decreasing LSN across the 256 MiB file.
 */
struct alignas(8) WalRecordHeader {
    uint64_t lsn;           // [0x00..0x07] Monotonic LSN or Hybrid Logical Clock (HLC)
    uint32_t record_crc;    // [0x08..0x0B] Non-zero CRC32C(seed=epoch_lo32, lsn + [0x0C..0x0F] + payload)
    uint16_t len_and_type;  // [0x0C..0x0D] Low 12 bits: payload_len (0..4064); High 4 bits: RecordType
    uint8_t  flags;         // [0x0E]       Bitmask from RecordFlags
    uint8_t  user_meta;     // [0x0F]       Storage engine tag (e.g., Table ID, OpCode)

    [[nodiscard]] static constexpr uint16_t PackLenAndType(uint16_t len, RecordType type) noexcept {
        assert(len <= MAX_FRAG_PAYLOAD);
        return static_cast<uint16_t>((len & 0x0FFFU) | (static_cast<uint16_t>(type) << 12U));
    }

    [[nodiscard]] constexpr uint16_t PayloadLen() const noexcept {
        return static_cast<uint16_t>(len_and_type & 0x0FFFU);
    }

    [[nodiscard]] constexpr RecordType Type() const noexcept {
        return static_cast<RecordType>(len_and_type >> 12U);
    }
};
static_assert(sizeof(WalRecordHeader) == REC_HDR_SIZE, "WalRecordHeader must be 16 bytes");

// ============================================================================
// SECTION 3: HARDWARE-ACCELERATED CHECKSUMS (google/crc32c)
// ============================================================================

/**
 * @brief Deterministic hardware-accelerated 64-bit checksum backed by
 *        `google/crc32c` (`Crc32c::crc32c`), used for `WalSegmentHeader` and
 *        the 8-byte payload of `WalBlockHeader`.
 */
[[nodiscard]] inline uint64_t ComputeWalChecksum64(const void* data, size_t len) noexcept {
    const auto*    bytes = static_cast<const uint8_t*>(data);
    const uint32_t crc_a = crc32c::Crc32c(bytes, len);
    const uint32_t crc_b = crc32c::Extend(0x9E3779B9U ^ static_cast<uint32_t>(len), bytes, len);
    uint64_t       combined = (static_cast<uint64_t>(crc_a) << 32U) | static_cast<uint64_t>(crc_b);
    combined ^= combined >> 33U;
    combined *= 0xff51afd7ed558ccdULL;
    combined ^= combined >> 33U;
    return combined == 0 ? 1ULL : combined;
}

/**
 * @brief Hardware-accelerated per-fragment CRC32C seeded with `segment_epoch`
 *        (lower 32 bits), covering `hdr.lsn`, `hdr.len_and_type`, `hdr.flags`,
 *        `hdr.user_meta`, and the fragment `payload`.
 *
 * Guarantees a non-zero return value (`0` is reserved for zero padding).
 */
[[nodiscard]] inline uint32_t ComputeWalRecordCrc32(
    uint32_t               epoch_lo32,
    const WalRecordHeader& hdr,
    const void*            payload,
    size_t                 payload_len
) noexcept {
    const auto* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
    // Seed with epoch_lo32, cover lsn [0..7] and [12..15] (skipping record_crc at [8..11]).
    uint32_t crc = crc32c::Extend(epoch_lo32, hdr_bytes, 8);
    crc          = crc32c::Extend(crc, hdr_bytes + 12, 4);
    if (payload_len > 0) {
        crc = crc32c::Extend(crc, static_cast<const uint8_t*>(payload), payload_len);
    }
    return crc == 0 ? 1U : crc;
}

// ============================================================================
// SECTION 4: READER -> WRITER HANDOFF CONTRACT (`RecoveredTailState`)
// ============================================================================

/**
 * @brief Complete segment & sub-sector tail state produced by `WalReader` and
 *        consumed by `WalWriter::ResumeFromTail()` to resume appending in-place.
 *
 * Layout Note:
 *   `tail_sector_data` (4096 B) is placed at byte offset 0 followed by the
 *   64-byte scalar metadata line so `sizeof(RecoveredTailState) == 4160` bytes
 *   with zero internal alignment holes.
 *
 * Design Rule:
 *   Only independent facts are stored; every derivable value is a method.
 *   Append-in-place eligibility used to be a stored `bool` denormalized from
 *   `valid_bytes_in_tail` — a stale or hand-built mismatch could silently
 *   resume the writer at the wrong block. As a method it cannot lie, and the
 *   scalar line lands at exactly 64 bytes.
 */
struct alignas(64) RecoveredTailState {
    // Raw 4KB bytes of `last_valid_block_idx` captured during recovery (with any
    // trailing uncommitted `FIRST` fragment already stripped and zero-padded).
    uint8_t tail_sector_data[BLOCK_SIZE]{};

    uint32_t shard_id = 0;             // From WalSegmentHeader::shard_id
    uint16_t valid_bytes_in_tail = 0;  // 0..4080 active committed payload bytes in tail block
    uint16_t tail_first_rec_offset = NO_NEW_RECORD;           // WalBlockHeader::first_rec_offset of tail block
    uint64_t segment_id = 0;     // From WalSegmentHeader::segment_id
    uint64_t segment_epoch = 1;  // From WalSegmentHeader::segment_epoch
    uint64_t base_lsn = 1;       // From WalSegmentHeader::base_lsn
    uint64_t total_segment_blocks = DEFAULT_SEG_BLOCKS;             // From WalSegmentHeader::segment_size_blocks
    uint64_t last_valid_block_idx = 0;  // 0 if segment has no valid data blocks
    uint64_t durable_lsn = 0;           // Highest complete record LSN on disk
    uint64_t next_lsn_to_assign = 1;    // `durable_lsn + 1` (or `base_lsn` if empty)

    /// True when the tail block holds committed bytes AND still has room for
    /// at least one minimal fragment: `WalWriter` then rewrites Block K in
    /// place instead of sealing it and appending at Block K + 1.
    [[nodiscard]] constexpr bool CanAppendInPlace() const noexcept {
        return valid_bytes_in_tail > 0 && MAX_BLOCK_PAYLOAD - valid_bytes_in_tail >= MIN_REC_SPACE;
    }
};
static_assert(sizeof(RecoveredTailState) == BLOCK_SIZE + 64,
              "RecoveredTailState must pack into 4096B sector + one 64B cache line");

// ============================================================================
// SECTION 5: DEDICATED `WalWriter` ENGINE
// ============================================================================

/**
 * @brief Dedicated, zero-virtual per-core WAL Writer built on `tinyio::IO`.
 *
 * How `WalWriter` Exploits `tinyio::IO::Tick()` & `tinyio::Baton`:
 * +--------------------------------------------------------------------------+
 * |                Single `tinyio::IO::Tick()` Execution Cycle               |
 * +------------------------+------------------------+------------------------+
 * | 1. Worker Fibers 1..N  | 2. FlusherFiber runs   | 3. SubmitOrWait()      |
 * | stage into Buffer[0],  | from ready_queue_,     | submits WRITE_FIXED    |
 * | WakeFlusherIfNeeded(), | swaps Buffer[0]<->[1], | SQE to NVMe in the     |
 * | and wait on Baton      | calls io_.WriteFixed() | exact same tick!       |
 * +------------------------+------------------------+------------------------+
 */

/**
 * @brief Stack-allocated wait node for `WalWriter::AppendAndSync()` group commit.
 */
struct SyncWaiterNode {
    tinyio::Baton   done;
    SyncWaiterNode* next = nullptr;
    explicit SyncWaiterNode(tinyio::IO& io) noexcept : done(io) {}
};

/**
 * @brief Stack-allocated wait node for staging admission / buffer space overflow.
 */
struct SpaceWaiterNode {
    tinyio::Baton    done;
    SpaceWaiterNode* next = nullptr;
    explicit SpaceWaiterNode(tinyio::IO& io) noexcept : done(io) {}
};

class WalWriter {
private:
    // ------------------------------------------------------------------------
    // 1. Runtime & Segment Identity
    // ------------------------------------------------------------------------
    tinyio::IO&      io_;                                         // Owning thread-per-core tinyio runtime
    tinyio::UniqueFd fd_{};                                       // Open segment file descriptor (O_RDWR | O_DIRECT | O_DSYNC)
    uint32_t         shard_id_             = 0;                   // Defaults to io_.Id()
    int              fatal_io_err_         = 0;                   // Latched errno on WriteFixed failure (0 == healthy)
    uint64_t         segment_id_           = 0;                   // Monotonic logical segment number
    uint64_t         segment_epoch_        = 1;                   // Physical reuse generation of this segment file
    uint64_t         total_segment_blocks_ = DEFAULT_SEG_BLOCKS;  // Total 4KB blocks in segment (including Block 0)

    // ------------------------------------------------------------------------
    // 2. LSN Watermarks
    // ------------------------------------------------------------------------
    uint64_t base_lsn_            = 1;  // First LSN written at Block 1 of this segment
    uint64_t next_lsn_            = 1;  // Next LSN to assign once admitted to StageRecord()
    uint64_t staged_complete_lsn_ = 0;  // Highest LSN whose FULL/LAST fragment is completely staged in active buffer
    uint64_t durable_lsn_         = 0;  // Highest LSN confirmed durable by WriteFixed()

    // ------------------------------------------------------------------------
    // 3. Ping-Pong Registered DMA Buffers & Sub-Sector Staging Cursors
    // ------------------------------------------------------------------------
    tinyio::FixedBuffer dma_buffers_[2]{};                       // Two 256 KiB registered FixedBuffer leases
    size_t              blocks_per_dma_buf_   = 0;               // dma_buffers_[0].Size() / 4096 (e.g., 64 blocks)
    uint8_t             active_buf_idx_       = 0;               // 0 or 1: buffer where fibers currently stage
    bool                io_in_flight_         = false;           // True while FlusherFiberLoop is inside WriteFixed()
    bool                staging_busy_         = false;           // True while a fiber is inside StageRecord() (prevents mid-record interleaving)
    uint64_t            buf_start_disk_block_ = 1;               // Disk 4KB block index corresponding to Block 0 of active buffer
    size_t              cur_buf_block_        = 0;               // Current 4KB block index (0 .. blocks_per_dma_buf_)
    uint16_t            cur_block_offset_     = BLOCK_HDR_SIZE;  // Write cursor inside current 4KB block (16 .. 4096)

    // ------------------------------------------------------------------------
    // 4. Dedicated Flusher Fiber & Zero-Allocation Sync Primitives
    // ------------------------------------------------------------------------
    // The flusher sleeps on a fresh one-shot Baton published through
    // `flusher_wake_` (re-check predicate, poster clears pointer with
    // std::exchange before Post()).
    tinyio::Baton*   flusher_wake_              = nullptr;  // Sleeping flusher's baton; null while busy
    tinyio::Baton*   flusher_exit_waiter_       = nullptr;  // Fiber parked inside Shutdown() waiting for flusher exit
    bool             shutting_down_             = false;    // Set by Shutdown() to break FlusherFiberLoop()
    bool             flusher_exited_            = true;     // False while FlusherFiberLoop() is alive
    SyncWaiterNode*  sync_waiters_[2]{};                    // Intrusive stack lists paired with dma_buffers_[0..1]
    SpaceWaiterNode* space_overflow_head_       = nullptr;  // FIFO head of fibers waiting for staging admission
    SpaceWaiterNode* space_overflow_tail_       = nullptr;  // FIFO tail of fibers waiting for staging admission

public:
    explicit WalWriter(tinyio::IO& io) noexcept
        : io_(io), shard_id_(static_cast<uint32_t>(io.Id())) {}

    ~WalWriter() noexcept {
        if (!flusher_exited_ || sync_waiters_[0] != nullptr || sync_waiters_[1] != nullptr ||
            space_overflow_head_ != nullptr) {
            ELOGFMT(FATAL,
                    "WalWriter destroyed on shard {} without calling Shutdown() "
                    "(flusher_exited={})",
                    shard_id_, flusher_exited_);
            std::abort();
        }
    }

    WalWriter(const WalWriter&)            = delete;
    WalWriter& operator=(const WalWriter&) = delete;
    WalWriter(WalWriter&&)                 = delete;
    WalWriter& operator=(WalWriter&&)      = delete;

    // ------------------------------------------------------------------------
    // Watermark & Capacity Accessors
    // ------------------------------------------------------------------------
    [[nodiscard]] constexpr uint64_t DurableLsn() const noexcept { return durable_lsn_; }
    [[nodiscard]] constexpr uint64_t NextLsn() const noexcept { return next_lsn_; }
    [[nodiscard]] constexpr uint64_t SegmentId() const noexcept { return segment_id_; }
    [[nodiscard]] constexpr uint64_t SegmentEpoch() const noexcept { return segment_epoch_; }
    [[nodiscard]] constexpr int      FatalIoError() const noexcept { return fatal_io_err_; }

    [[nodiscard]] constexpr uint64_t CurrentDiskBlock() const noexcept {
        return buf_start_disk_block_ + cur_buf_block_;
    }

    /**
     * @brief Exact check for whether appending a record of `incoming_bytes`
     *        would exceed `total_segment_blocks_`.
     */
    [[nodiscard]] constexpr bool IsSegmentFull(size_t incoming_bytes) const noexcept {
        const bool   can_start_in_cur = (cur_buf_block_ < blocks_per_dma_buf_) &&
                                        ((BLOCK_SIZE - cur_block_offset_) >= MIN_REC_SPACE);
        const size_t avail_in_cur     = can_start_in_cur
                                            ? (BLOCK_SIZE - cur_block_offset_ - REC_HDR_SIZE)
                                            : 0;
        size_t extra_blocks = 0;
        if (!can_start_in_cur || incoming_bytes > avail_in_cur) {
            const size_t rem = incoming_bytes - avail_in_cur;
            extra_blocks     = (rem == 0) ? 1U : ((rem + MAX_FRAG_PAYLOAD - 1U) / MAX_FRAG_PAYLOAD);
        }
        return (CurrentDiskBlock() + extra_blocks) >= total_segment_blocks_;
    }

    // ------------------------------------------------------------------------
    // Lifecycle & Segment Initialization / Recovery Handoff
    // ------------------------------------------------------------------------

    /**
     * @brief Lease two `FixedBuffer` slots from `io_.TakeFixedBuffer()` and
     *        spawn the background `FlusherFiberLoop()` fiber.
     */
    [[nodiscard]] tinyio::Result<void> Start() noexcept;

    /**
     * @brief Drain any pending writes, cleanly exit `FlusherFiberLoop()` (so
     *        `tinyio::IO::AbortIfParkedFibers()` is satisfied), and release
     *        both `FixedBuffer` leases back to the pool.
     */
    [[nodiscard]] tinyio::Result<void> Shutdown() noexcept;

    /**
     * @brief Format a fresh or recycled pre-allocated segment file by writing
     *        Block 0 (`WalSegmentHeader`) with `new_epoch` (`uint32_t(new_epoch) != 0`)
     *        and issuing an `Fsync(fd_, true)` barrier before any data blocks are staged.
     */
    [[nodiscard]] tinyio::Result<void> InitOrRecycleSegment(
        tinyio::UniqueFd fd,
        uint64_t         segment_id,
        uint64_t         new_epoch,
        uint64_t         base_lsn,
        uint64_t         prev_segment_max_lsn = 0,
        uint64_t         total_blocks         = DEFAULT_SEG_BLOCKS
    ) noexcept;

    /**
     * @brief Zero-read handoff from `WalReader`: adopt the open segment `fd`,
     *        wipe any post-tail DMA tear window so out-of-order phantom sectors
     *        cannot resurrect across a second crash, and prime `dma_buffers_[0]`
     *        + sub-sector write cursors directly from `RecoveredTailState`.
     *
     * Trust Boundary: before copying, CHECK that `tail_sector_data` bytes
     * `[BLOCK_HDR_SIZE + valid_bytes_in_tail .. 4095]` are all zero — the
     * zero-pad half of the Phase-4 contract — so a botched strip aborts loudly
     * here instead of writing resurrected torn data to NVMe. The CHECK is what
     * lets `CanAppendInPlace()` trust `valid_bytes_in_tail` unconditionally.
     */
    [[nodiscard]] tinyio::Result<void> ResumeFromTail(
        tinyio::UniqueFd          fd,
        const RecoveredTailState& tail_state
    ) noexcept;

    /**
     * @brief Release ownership of the underlying `UniqueFd` (only valid when
     *        no writes are staged or in flight).
     */
    [[nodiscard]] tinyio::UniqueFd ReleaseFd() noexcept { return std::move(fd_); }

    // ------------------------------------------------------------------------
    // Hot-Path Group Commit Write API
    // ------------------------------------------------------------------------

    /**
     * @brief Stage a record into the active `FixedBuffer` (respecting strict
     *        FIFO overflow admission) and suspend the calling fiber on
     *        `sync_waiters_[buf_idx]` via a stack-allocated `SyncWaiterNode`
     *        until the NVMe group-commit batch completes durability.
     */
    [[nodiscard]] tinyio::Result<uint64_t> AppendAndSync(
        std::span<const std::byte> payload,
        uint8_t                    flags     = RecordFlags::NONE,
        uint8_t                    user_meta = 0
    ) noexcept;

private:
    [[nodiscard]] uint8_t* BlockPtr(uint8_t buf_idx, size_t blk_idx) noexcept {
        assert(blk_idx < blocks_per_dma_buf_);
        return reinterpret_cast<uint8_t*>(dma_buffers_[buf_idx].Ptr() + (blk_idx * BLOCK_SIZE));
    }

    [[nodiscard]] uint8_t* ActiveBlockPtr() noexcept {
        return BlockPtr(active_buf_idx_, cur_buf_block_);
    }

    [[nodiscard]] constexpr bool HasStagedBytes() const noexcept {
        return cur_buf_block_ > 0 || cur_block_offset_ > BLOCK_HDR_SIZE;
    }

    void WakeFlusherIfNeeded() noexcept {
        if (flusher_wake_ != nullptr) {
            std::exchange(flusher_wake_, nullptr)->Post();
        }
    }

    /**
     * @brief Initialize the immutable 16-byte `WalBlockHeader` at byte 0 of
     *        `blk_ptr` when the first fragment (`first_type`, `first_payload_len`)
     *        is placed at byte offset 16. Never called again on that block.
     */
    void InitBlockHeader(uint8_t* blk_ptr, RecordType first_type, uint16_t first_payload_len) noexcept;

    /**
     * @brief Stage `payload` into `dma_buffers_[active_buf_idx_]`, splitting
     *        across 4KB boundaries into self-checksummed `FULL`, `FIRST`,
     *        `MIDDLE`, `LAST` fragments. May suspend inside `EnsureBufferSpace()`
     *        if the record spans across a 256 KiB DMA buffer boundary while
     *        holding `staging_busy_`.
     */
    [[nodiscard]] tinyio::Result<uint64_t> StageRecord(
        std::span<const std::byte> payload,
        uint8_t                    flags,
        uint8_t                    user_meta
    ) noexcept;

    void FlusherFiberLoop() noexcept;

    /**
     * @brief Zero-pad any unwritten tail of the last active 4KB block, flip
     *        `active_buf_idx_ ^= 1`, copy any partial 4KB tail prefix
     *        `[0..cur_block_offset_)` into Block 0 of the new buffer, and
     *        return the index of the flushing buffer (`0` or `1`).
     */
    [[nodiscard]] uint8_t SwapAndPrepareFlush(
        size_t&   out_bytes_to_write,
        off_t&    out_disk_byte_offset,
        uint64_t& out_batch_max_lsn
    ) noexcept;

    void AdvanceBlock() noexcept;
    [[nodiscard]] tinyio::Result<void> AcquireStagingAdmission() noexcept;
    [[nodiscard]] tinyio::Result<void> EnsureBufferSpace() noexcept;
    void WakeNextOverflowWaiter() noexcept;
    void DrainSyncWaiters(uint8_t buf_idx) noexcept;
};

// ============================================================================
// SECTION 6: WAL READER & CRASH-RECOVERY SPECIFICATION (`WalReader`)
// ============================================================================
/**
 * Resource Use: Exactly 1 `tinyio::FixedBuffer` lease, 0 background fibers.
 *
 * ----------------------------------------------------------------------------
 * 6.1. ALGORITHM 2: CRASH-SAFE BINARY SEARCH + TEAR-WINDOW VERIFICATION
 * ----------------------------------------------------------------------------
 * When recovering a pre-allocated segment file after power loss, blocks were
 * NOT zero-filled before the current `segment_epoch` started. Moreover, if a
 * multi-block 256 KiB `WriteFixed` DMA was in flight when power was cut, the
 * NVMe drive may have committed higher LBAs to flash before lower LBAs
 * ("out-of-order multi-block DMA tear"):
 *
 * Physical Disk Layout After an Out-of-Order Multi-Block DMA Crash:
 * +--------------------------------------------------------------------------+
 * |             Recycled Segment File (Epoch E = 7, Prior E = 6)             |
 * +-------------+----------------+-------------+--------------+--------------+
 * |   Block 0   | Blocks 1..300  |  Block 301  |  Block 302   | Blks 303..N  |
 * | SegHdr E=7  | Committed E=7  |  Torn! E=6  | Out-of-Order | Stale E=6 or |
 * |   [VALID]   |    [VALID]     |  [INVALID]  | E=7 [VALID!] | 0 [INVALID]  |
 * +-------------+----------------+-------------+--------------+--------------+
 *                                ^             ^
 *                                |             +-- Uncommitted in-flight LBA!
 *                                +-- True Crash Boundary (Must stop at 300!)
 *
 * Why Naive Binary Search Fails Here (And How the 4-Phase Reader Solves It):
 *   If a naive binary search probes `mid = 302`, `IsBlockValid(302, E=7)`
 *   returns `TRUE`! A naive binary search would then search `[303..N]`,
 *   missing the torn hole at Block 301 and replaying uncommitted data from
 *   Block 302.
 *
 *   Because `WalWriter` has at most ONE `WriteFixed` of size `<= 256 KiB`
 *   (64 blocks) in flight at a time, every valid block of Epoch `E` lies
 *   within `blocks_per_buf_` (64 blocks) of the true contiguous boundary `K`.
 *
 * The 4-Phase Recovery Pipeline in `WalReader::RecoverSegmentTail()`:
 * +--------------------------------------------------------------------------+
 * |             WalReader::RecoverSegmentTail() 4-Phase Pipeline             |
 * +-----------------+-----------------+------------------+-------------------+
 * | Phase 1: Blk 0  | Phase 2: Coarse | Phase 3: Linear  | Phase 4: Sanitize |
 * | Validate Magic, | O(log N) BSearch| Tear-Window Scan | 4KB Tail Sector & |
 * | Version & CRC   | in <= 16 Reads  | [cand-64..cand+64| Handle Aborted    |
 * | Extract Epoch E | Finds `cand`    | Stops @ 1st Hole | Multi-Blk Record  |
 * +-----------------+-----------------+------------------+-------------------+
 *
 * Phase 4 Tail Sector Sanitization & Aborted Multi-Block Record Rules:
 *   1. Walk fragments inside `last_valid_block_idx` (`Block K`) starting at
 *      byte offset 16, verifying `IsFragmentValid()` and strict LSN continuity
 *      until hitting `RecordType::PADDING` (`0`) or a torn trailing fragment
 *      at `valid_end_offset`:
 *        - Zero-fill `[valid_end_offset .. 4095]` inside `tail_sector_data`.
 *   2. If the last valid fragment in `Block K` is `FIRST` or `MIDDLE` (an
 *      uncommitted multi-block record `lsn = M` whose `LAST` fragment never
 *      reached disk):
 *        - Sub-case A (`FIRST(M)` started at `offset > 16` inside `Block K`
 *          after committed records `[16..offset)`):
 *          Strip `FIRST(M)` by zeroing `[offset .. 4095]` in `tail_sector_data`,
 *          set `valid_bytes_in_tail = offset - 16`, `durable_lsn = M - 1`, and
 *          `next_lsn_to_assign = M`. `CanAppendInPlace()` needs no explicit
 *          setting: `MAX_BLOCK_PAYLOAD - (offset - 16) == 4096 - offset`, so
 *          it evaluates true exactly when `4096 - offset >= MIN_REC_SPACE`.
 *        - Sub-case B (`FIRST(M)` or `MIDDLE(M)` started at offset 16 of
 *          `Block K`, so `Block K` contains no committed record boundary):
 *          Do NOT rewind `last_valid_block_idx` across earlier blocks (which
 *          would leave `> 64` blocks of `Epoch E` beyond the cursor). Keep
 *          `last_valid_block_idx = K` and set `valid_bytes_in_tail = 0`
 *          (rule 1's zero-fill already emptied `[16 .. 4095]`, so
 *          `CanAppendInPlace()` is false by construction and `WalWriter`
 *          appends at `Block K + 1`), set `durable_lsn = M - 1`, and set
 *          `next_lsn_to_assign = M + 1` (preserving monotonic block LSNs).
 *
 * ----------------------------------------------------------------------------
 * 6.2. CHECKPOINT LSN BINARY SEARCH (`WalReader::FindBlockForLsn`)
 * ----------------------------------------------------------------------------
 * Because EVERY fragment header (`FULL`, `FIRST`, `MIDDLE`, `LAST`) at byte
 * offset 16 of every active 4KB block carries its record's `lsn`, we can
 * binary-search `[1 .. tail.last_valid_block_idx]` in `<= 16` reads.
 *
 * Exact Lower-Bound Rule (Avoiding the Mid-Block Start Trap):
 *   Let `U` be the first block in `[1 .. tail.last_valid_block_idx]` whose
 *   fragment at offset 16 has `hdr_at_16.lsn >= target_lsn`:
 *     - If no such `U` exists -> return `tail.last_valid_block_idx`.
 *     - If `U > 1` and (`hdr_at_16.lsn > target_lsn` OR
 *       `hdr_at_16.Type() == RecordType::MIDDLE` OR
 *       `hdr_at_16.Type() == RecordType::LAST`):
 *       `target_lsn` started at a later offset inside block `U - 1` -> return `U - 1`.
 *     - Otherwise -> return `U`.
 *
 * ----------------------------------------------------------------------------
 * 6.3. ALGORITHM 3: MULTI-BLOCK FRAGMENT REASSEMBLY (`ReplaySegment`)
 * ----------------------------------------------------------------------------
 * Streams 256 KiB chunks via `io_.ReadFixed()` and drives the 4-state fragment
 * reassembly machine:
 *
 * +--------------------------------------------------------------------------+
 * |                 FragmentAssembler State Machine Transitions              |
 * +------------------------+------------------------+------------------------+
 * |   RecordType::FULL     |   RecordType::FIRST    |  MIDDLE* -> LAST       |
 * | Deliver span directly  | Reset buffer, copy 1st | Append payload chunks; |
 * | Zero vector copy!      | chunk, set in_progress | Deliver on LAST!       |
 * +------------------------+------------------------+------------------------+
 *   Note: If `in_progress` is true (from an uncommitted `FIRST`/`MIDDLE` chain
 *   prior to a crash recovery) and a new `FULL` or `FIRST` fragment arrives,
 *   `FragmentAssembler` calls `Reset()` to discard the aborted partial record
 *   before processing the new record.
 */

/**
 * @brief Callback invoked by `WalReader::ReplaySegment()` for each complete
 *        reconstructed record with `lsn >= min_lsn_to_replay`.
 *
 * @return `true` to continue replaying, or `false` to stop replay early.
 */
using ReplayCallback = bool (*)(
    void*                      ctx,
    uint64_t                   lsn,
    std::span<const std::byte> payload,
    uint8_t                    flags,
    uint8_t                    user_meta
);

/**
 * @brief Standalone, single-buffer WAL crash recovery & log replay reader.
 */
class WalReader {
public:
    /**
     * @brief State machine context for Algorithm 3 (Multi-Block Fragment Reassembly).
     */
    struct FragmentAssembler {
        std::vector<std::byte> buffer{};              // Reassembly buffer for multi-block records
        uint64_t               expected_lsn = 0;      // LSN captured from the FIRST fragment
        uint8_t                flags        = 0;      // Flags captured from the FIRST fragment
        uint8_t                user_meta    = 0;      // User metadata captured from the FIRST fragment
        bool                   in_progress  = false;  // True while accumulating FIRST -> MIDDLE* -> LAST

        void Reset() noexcept {
            buffer.clear();
            expected_lsn = 0;
            flags        = 0;
            user_meta    = 0;
            in_progress  = false;
        }
    };

private:
    tinyio::IO&         io_;                    // Owning thread-per-core tinyio runtime
    tinyio::FixedBuffer read_buf_{};            // Single leased registered DMA buffer (e.g. 256 KiB)
    size_t              blocks_per_buf_ = 0;    // `read_buf_.Size() / BLOCK_SIZE` (e.g. 64 blocks)

public:
    explicit WalReader(tinyio::IO& io) noexcept : io_(io) {}

    ~WalReader() noexcept { ReleaseBuffer(); }

    WalReader(const WalReader&)            = delete;
    WalReader& operator=(const WalReader&) = delete;
    WalReader(WalReader&&)                 = delete;
    WalReader& operator=(WalReader&&)      = delete;

    // ------------------------------------------------------------------------
    // Buffer Lease Lifecycle
    // ------------------------------------------------------------------------

    /**
     * @brief Lease a single registered `FixedBuffer` slot from `io_`.
     *
     * @param wait_for_slot If true, calls `io_.AcquireFixedBuffer()` (parks
     *                      calling fiber if pool is empty). If false, calls
     *                      non-blocking `io_.TakeFixedBuffer()`.
     */
    [[nodiscard]] tinyio::Result<void> AcquireBuffer(bool wait_for_slot = false) noexcept;

    /**
     * @brief Return the leased `FixedBuffer` slot to `tinyio::FixedBufferPool`.
     */
    void ReleaseBuffer() noexcept {
        read_buf_.Release();
        blocks_per_buf_ = 0;
    }

    [[nodiscard]] bool HasBuffer() const noexcept { return read_buf_.Valid(); }

    // ------------------------------------------------------------------------
    // Crash Recovery, Checkpoint Seeking & Streaming Replay API
    // ------------------------------------------------------------------------

    /**
     * @brief Execute Algorithm 2 (4-Phase Crash-Safe Binary Search + Tear-Window
     *        Scan + Tail Sanitization) on `fd` and return a `RecoveredTailState`
     *        ready for `ReplaySegment()` or `WalWriter::ResumeFromTail()`.
     *
     * Does NOT take ownership of `fd`, allowing the caller to pass the same
     * `fd` to `ReplaySegment()` and then move it into `WalWriter::ResumeFromTail()`.
     */
    [[nodiscard]] tinyio::Result<RecoveredTailState> RecoverSegmentTail(
        const tinyio::UniqueFd& fd
    ) noexcept;

    /**
     * @brief Binary-search `[1 .. tail.last_valid_block_idx]` in `O(log N)`
     *        4KB `ReadFixed` calls to find the exact block where replay must
     *        begin to reconstruct `target_lsn` (stepping back to `U - 1` when
     *        `target_lsn` started mid-block in `U - 1`).
     */
    [[nodiscard]] tinyio::Result<uint64_t> FindBlockForLsn(
        const tinyio::UniqueFd&   fd,
        const RecoveredTailState& tail,
        uint64_t                  target_lsn
    ) noexcept;

    /**
     * @brief Execute Algorithm 3 (Batched 256 KiB `ReadFixed` Streaming +
     *        Multi-Block Fragment Reassembly) from `min_lsn_to_replay` through
     *        `tail.last_valid_block_idx`.
     */
    [[nodiscard]] tinyio::Result<void> ReplaySegment(
        const tinyio::UniqueFd&   fd,
        const RecoveredTailState& tail,
        uint64_t                  min_lsn_to_replay,
        ReplayCallback            callback,
        void*                     callback_ctx
    ) noexcept;

    /**
     * @brief Validates whether a single record fragment at `frag_offset` inside
     *        `blk_ptr` has in-bounds geometry and passes `ComputeWalRecordCrc32`
     *        for `expected_epoch_low32`.
     */
    [[nodiscard]] static bool IsFragmentValid(
        const uint8_t* blk_ptr,
        uint16_t       frag_offset,
        uint32_t       expected_epoch_low32
    ) noexcept {
        if (frag_offset < BLOCK_HDR_SIZE || frag_offset + REC_HDR_SIZE > BLOCK_SIZE) {
            return false;
        }
        WalRecordHeader hdr{};
        std::memcpy(&hdr, blk_ptr + frag_offset, REC_HDR_SIZE);
        const RecordType type        = hdr.Type();
        const uint16_t   payload_len = hdr.PayloadLen();
        if (type < RecordType::FULL || type > RecordType::LAST || hdr.record_crc == 0) {
            return false;
        }
        if (frag_offset + REC_HDR_SIZE + payload_len > BLOCK_SIZE) {
            return false;
        }
        const uint32_t expected_crc = ComputeWalRecordCrc32(
            expected_epoch_low32, hdr, blk_ptr + frag_offset + REC_HDR_SIZE, payload_len);
        return hdr.record_crc == expected_crc;
    }

    /**
     * @brief Validates whether a 4096-byte sector in memory belongs to
     *        `expected_epoch_low32` (valid immutable `WalBlockHeader` AND at
     *        least one valid self-checksummed fragment at byte offset 16).
     */
    [[nodiscard]] static bool IsBlockValid(
        const uint8_t* blk_ptr,
        uint32_t       expected_epoch_low32
    ) noexcept {
        WalBlockHeader blk_hdr{};
        std::memcpy(&blk_hdr, blk_ptr, BLOCK_HDR_SIZE);
        if (blk_hdr.segment_epoch != expected_epoch_low32 || blk_hdr.reserved != 0) {
            return false;
        }
        if (blk_hdr.block_header_crc != ComputeWalChecksum64(blk_ptr + 8, 8)) {
            return false;
        }
        return IsFragmentValid(blk_ptr, static_cast<uint16_t>(BLOCK_HDR_SIZE), expected_epoch_low32);
    }

private:
    [[nodiscard]] uint8_t* BlockPtr(size_t blk_idx) noexcept {
        assert(blk_idx < blocks_per_buf_);
        return reinterpret_cast<uint8_t*>(read_buf_.Ptr() + (blk_idx * BLOCK_SIZE));
    }

    [[nodiscard]] const uint8_t* BlockPtr(size_t blk_idx) const noexcept {
        assert(blk_idx < blocks_per_buf_);
        return reinterpret_cast<const uint8_t*>(read_buf_.Ptr() + (blk_idx * BLOCK_SIZE));
    }
};

}  // namespace storage::wal::split
