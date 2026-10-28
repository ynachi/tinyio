//
// Shared constructs between Reader and Writer, like config, formats, ...
//

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "tinyio/error.hpp"
#include "tinyio/net/fd.hpp"
#include "tinyio/runtime/buffer_pool.hpp"
#include "tinyio/runtime/fiber_sync.hpp"
#include "tinyio/runtime/runtime.hpp"

namespace tinyio::wal {
// ============================================================================
// SECTION 0: CONSTANTS, SYSTEM LIMITS, CONFIGS
// ============================================================================

inline constexpr uint64_t WAL_MAGIC          = 0x57414C5F32303236ULL; // "WAL_2026"
inline constexpr uint32_t WAL_FORMAT_VER     = 1;

inline constexpr size_t   WAL_BLOCK_SIZE         = 4096;
inline constexpr size_t   BLOCK_HDR_SIZE     = 16;
inline constexpr size_t   REC_HDR_SIZE       = 16;
inline constexpr size_t   MAX_BLOCK_PAYLOAD  = WAL_BLOCK_SIZE - BLOCK_HDR_SIZE; // 4080 B
inline constexpr size_t   MIN_REC_SPACE      = REC_HDR_SIZE + 1;            // 17 B

// Default FixedBuffer slot size expected from `tinyio::IoOptions::buffer_pool`:
// 64 blocks * 4096B = 256 KiB per FixedBuffer lease (divides 1 GiB evenly!).
inline constexpr size_t   DEFAULT_DMA_BLOCKS = 64;
inline constexpr size_t   DEFAULT_DMA_BYTES  = DEFAULT_DMA_BLOCKS * WAL_BLOCK_SIZE;

// Default segment file size: 256 MiB = 65,536 blocks of 4KB
inline constexpr uint64_t DEFAULT_SEG_BLOCKS = 65536;

// Sentinel value for WalBlockHeader::first_rec_offset when a 4KB block is 100%
// consumed by a MIDDLE or LAST fragment of a multi-block record.
inline constexpr uint16_t NO_NEW_RECORD      = 0xFFFF;

struct Options {
    std::filesystem::path directory;

    /// Size of each segment file, preallocated when the segment is created.
    /// The writer rotates when the next record would not fit. A multiple of
    /// kBlockSize.
    size_t segment_size = 64u << 20;

    size_t block_size = 4096;

    // Must be a multiple of kBlockSize. If its full before the user calls Sync, it will be
    // flushed to disk. And the durability counter will be updated.
    size_t scratch_buffer_size = 1u << 21;  // 2Mb

    mode_t file_mode = 0644;
    mode_t dir_mode = 0755;
    int read_flags = O_RDONLY;
    // TODO should fail when the file exist, since segment should not be reopened for writing.
    int write_flags = O_CREAT | O_RDWR | O_EXCL | O_DIRECT;

    /// O_DIRECT through the registered fixed buffer. Off only for file
    /// systems that refuse O_DIRECT (tmpfs), e.g. in tests; the format is
    /// the same either way.
    bool direct_io = true;
};

// ============================================================================
// SECTION 1: WIRE FORMAT STRUCTURES (4KB SECTOR ALIGNED, LITTLE-ENDIAN)
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
 * Written once when a segment is newly initialized or recycled via
 * `io_.WriteFixed(fd_, dma_buffers_[0], WAL_BLOCK_SIZE, 0)` + `io_.Fsync(fd_, true)`
 * BEFORE any data blocks (Blocks 1..N-1) for the new `segment_epoch` are written.
 */
struct alignas(WAL_BLOCK_SIZE) SegmentHeader {
    uint64_t magic;                         // [0x00..0x07] Must equal WAL_MAGIC
    uint32_t version;                       // [0x08..0x0B] Must equal WAL_FORMAT_VER
    uint32_t shard_id;                      // [0x0C..0x0F] Owning tinyio::IO::Id()
    uint64_t segment_id;                    // [0x10..0x17] Logical monotonically increasing segment number
    uint64_t segment_epoch;                 // [0x18..0x1F] Physical reuse counter of this pre-allocated file
    uint64_t base_lsn;                      // [0x20..0x27] First LSN expected at Block 1, Offset 16
    uint64_t prev_segment_max_lsn;          // [0x28..0x2F] Highest LSN committed in segment_id - 1 (0 if none)
    uint64_t segment_size_blocks;           // [0x30..0x37] Total 4KB blocks in file (including Block 0)
    uint64_t header_checksum;               // [0x38..0x3F] Checksum64 of bytes [0x00..0x37]
    uint8_t  reserved[WAL_BLOCK_SIZE - 64]; // Zero-filled padding to 4096 bytes
};
static_assert(sizeof(SegmentHeader) == WAL_BLOCK_SIZE, "SegmentHeader must be 1 physical 4KB block");

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
struct alignas(8) BlockHeader {
    uint64_t block_header_crc;  // [0x00..0x07] ComputeWalChecksum64 of bytes [0x08..0x0F]
    uint32_t segment_epoch;     // [0x08..0x0B] Lower 32 bits of WalSegmentHeader::segment_epoch (!= 0)
    uint16_t first_rec_offset;  // [0x0C..0x0D] Offset (16..4079) of first FULL/FIRST record, or NO_NEW_RECORD
    uint16_t reserved;          // [0x0E..0x0F] Must be 0
};
static_assert(sizeof(BlockHeader) == BLOCK_HDR_SIZE, "WalBlockHeader must be 16 bytes");

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
}

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
struct alignas(8) RecordHeader {
    uint64_t lsn;           // [0x00..0x07] Monotonic LSN or Hybrid Logical Clock (HLC)
    uint32_t record_crc;    // [0x08..0x0B] Non-zero CRC32C(seed=epoch_lo32, lsn + [0x0C..0x0F] + payload)
    uint16_t len_and_type;  // [0x0C..0x0D] Low 12 bits: payload_len (0..4064); High 4 bits: RecordType
    uint8_t  flags;         // [0x0E]       Bitmask from RecordFlags
    uint8_t  user_meta;     // [0x0F]       Storage engine tag (e.g., Table ID, OpCode)

    [[nodiscard]] static constexpr uint16_t PackLenAndType(const uint16_t len, RecordType type) noexcept {
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
static_assert(sizeof(RecordHeader) == REC_HDR_SIZE, "WalRecordHeader must be 16 bytes");


/**
 * @brief Complete segment & sub-sector tail state produced by `WalReader` and
 *        consumed by `WalWriter::ResumeFromTail()` to resume appending in-place.
 *
 * Layout Note:
 *   `tail_sector_data` (4096 B) is placed at byte offset 0 followed by the
 *   64-byte scalar metadata line so `sizeof(RecoveredTailState) == 4160` bytes
 *   with zero internal alignment holes.
 */
struct alignas(64) RecoveredTailState {
    // Raw 4KB bytes of `last_valid_block_idx` captured during recovery (with any
    // trailing uncommitted `FIRST` fragment already stripped and zero-padded).
    uint8_t  tail_sector_data[WAL_BLOCK_SIZE]{};

    uint32_t shard_id                 = 0;                   // From WalSegmentHeader::shard_id
    uint16_t valid_bytes_in_tail      = 0;                   // 0..4080 active committed payload bytes in tail block
    uint16_t tail_first_rec_offset    = NO_NEW_RECORD;       // WalBlockHeader::first_rec_offset of tail block
    uint64_t segment_id               = 0;                   // From WalSegmentHeader::segment_id
    uint64_t segment_epoch            = 1;                   // From WalSegmentHeader::segment_epoch
    uint64_t base_lsn                 = 1;                   // From WalSegmentHeader::base_lsn
    uint64_t total_segment_blocks     = DEFAULT_SEG_BLOCKS;  // From WalSegmentHeader::segment_size_blocks
    uint64_t last_valid_block_idx     = 0;                   // 0 if segment has no valid data blocks
    uint64_t durable_lsn              = 0;                   // Highest complete record LSN on disk
    uint64_t next_lsn_to_assign       = 1;                   // `durable_lsn + 1` (or `base_lsn` if empty)
    bool     tail_can_append_in_place = false;               // True if `valid_bytes_in_tail > 0` and `4080 - valid_bytes_in_tail >= MIN_REC_SPACE`
};
static_assert(sizeof(RecoveredTailState) == WAL_BLOCK_SIZE + 64,
              "RecoveredTailState must pack into 4096B sector + one 64B cache line");
}