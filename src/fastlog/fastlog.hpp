#pragma once
#include <array>
#include <cstdint>
#include <filesystem>

#include "page_arena.hpp"
#include "sieve.hpp"
#include "tinyio/tinyio.hpp"

// Forward declare the test fixture in the global namespace
// so that the friend declaration works.
class AppendTest;
class ReadTest;
class CommitTest;
class ConcurrencyTest;

namespace fastlog {
enum class RecordOp : uint8_t { Upsert = 0, Delete = 1, Copy = 2 };

static constexpr std::size_t k_header_size = 16;

constexpr std::size_t AlignUpTo8(const std::size_t size) { return (size + 7) & ~7ULL; }

#pragma pack(push, 1)
struct RecordHeader {
  uint32_t crc32c;        // 4 bytes: CRC32c over [op, flags, payload_size, payload...]
                          // Must be computed last during write, after payload is copied.
  uint8_t op;             // 1 byte: RecordOp enum (TYPE)
  uint8_t flags;          // 1 byte: Room for flags (e.g. compressed, encrypted)
  uint32_t payload_size;  // 4 bytes: Size of the payload (Key + Value) only (SZ)
                          // Payload Layout: [ key_len (varint/fixed) ][ key bytes ... ][
                          // value bytes ... ]
  uint64_t address : 48;  // 6 bytes: 48-bit logical address. Used to definitively reject
                          // recycled space or torn writes without padding records.
};
#pragma pack(pop)

static_assert(sizeof(RecordHeader) == k_header_size,
              "Header must be exactly 16 bytes for alignment");

using LogAddress = uint64_t;

struct FastLogSettings {
  // ── Address geometry ──
  // A 64-bit logical address decodes by *threshold* (bits counted from 0,
  // cumulative — NOT three disjoint widths):
  //   offset_in_page    = addr & (2^page_bits    - 1)        // low page_bits
  //   page_number       = addr >> page_bits
  //   offset_in_segment = addr & (2^segment_bits - 1)        // CONTAINS the page bits
  //   segment_id        = addr >> segment_bits
  // Invariant: page_bits < segment_bits; a segment holds 2^(segment_bits-page_bits)
  // pages.
  uint page_bits = 22;        ///< default 4 MB pages  (FasterLog default is 25 = 32 MB)
  uint memory_bits = 26;      ///< default 64 MB resident -> 2^(26-22) = 16 pages in RAM
  uint segment_bits = 30;     ///< default 1 GB segment files on disk
  double mutable_frac = 0.9;  ///< default Last 90% of RAM stays unflushed until commit
  uint ro_page_cache = 128;   ///< How much page to keep in cache for reads, default 128
  std::filesystem::path dir;  ///< Path to physical segment files
  mode_t file_mode = 0644;    ///< Permission used when creating files
  mode_t dir_mode = 0755;     ///< Permission used when creating the main directory

  void Validate() const {
    auto valid_shift = [](const int b) {
      return b >= 0 && b < 63;
    };  // usable as a 64-bit shift amount
    if (!(valid_shift(page_bits) && (1ULL << page_bits) >= sizeof(RecordHeader))) {
      throw std::invalid_argument("page_bits");
    }
    if (!(segment_bits > page_bits && valid_shift(segment_bits))) {
      throw std::invalid_argument("segment_bits");
    }
    if (!(memory_bits >= page_bits + 1 && valid_shift(memory_bits))) {
      throw std::invalid_argument("memory_bits");
    }
    if (!(mutable_frac > 0.0 && mutable_frac <= 1.0)) {
      throw std::invalid_argument("mutable_frac");
    }
  }

  // we can derive many things at compile time from the settings !
  static constexpr uint64_t HeaderSize() { return sizeof(RecordHeader); }
  constexpr uint64_t PageSize() const { return 1ULL << page_bits; }
  constexpr uint64_t MemorySize() const { return 1ULL << memory_bits; }
  constexpr uint64_t SegmentSize() const { return 1ULL << segment_bits; }

  // compile time masks
  constexpr uint64_t PageMask() const { return PageSize() - 1; }
  constexpr uint64_t SegmentMask() const { return SegmentSize() - 1; }
  constexpr uint64_t MemoryMask() const { return MemorySize() - 1; }

  constexpr uint64_t NumPages() const { return 1ULL << (64 - page_bits); }
  constexpr uint64_t NumSegments() const { return 1ULL << (64 - segment_bits); }
  constexpr uint64_t NumPagesPerSegment() const {
    return 1ULL << (segment_bits - page_bits);
  }
  /// Pages in memory
  constexpr uint64_t ResidentPages() const { return 1ULL << (memory_bits - page_bits); }
  constexpr uint64_t SlotMask() const { return ResidentPages() - 1; }

  // Returns the physical file path for a given segment ID
  std::filesystem::path GetSegmentPath(uint64_t segment_id) const {
    return dir / std::format("{:08}.log", segment_id);
  }
};

/// @note AddressManager does not check any bound. It just do dumb maths.
/// So the caller MUST check bound.
///
class AddressManager {
  FastLogSettings s_;

  // The absolute start of the log (on disk - oldest retained data)
  LogAddress begin_ = 0;

  // Lowest address currently in RAM (oldest data in buffer, can be evicted up to
  // committed_)
  LogAddress head_ = 0;

  // Safely written to disk (buffer data older than this can be safely evicted)
  LogAddress committed_ = 0;

  // Where active appends happen (next unwritten byte in RAM)
  LogAddress tail_ = 0;

 public:
  AddressManager(const FastLogSettings& settings, const LogAddress first)
      : s_(settings), tail_(first) {}

  [[nodiscard]] constexpr std::uint64_t PageNumberOf(const LogAddress addr) const {
    return addr >> s_.page_bits;
  }
  [[nodiscard]] constexpr std::uint64_t OffsetInPageOf(const LogAddress addr) const {
    return addr & s_.PageMask();
  }
  [[nodiscard]] constexpr std::uint64_t SlotOf(const LogAddress addr) const {
    return PageNumberOf(addr) & s_.SlotMask();
  }
  [[nodiscard]] constexpr std::uint64_t SegmentNumberOf(const LogAddress addr) const {
    return addr >> s_.segment_bits;
  }
  [[nodiscard]] constexpr std::uint64_t OffsetInSegmentOf(const LogAddress addr) const {
    return addr & s_.SegmentMask();
  }
  [[nodiscard]] constexpr std::uint64_t OffsetInMemoryOf(const LogAddress addr) const {
    return addr & s_.MemoryMask();
  }
  [[nodiscard]] constexpr std::uint64_t PageNumberInSegmentOf(
      const LogAddress addr) const {
    return (addr & s_.SegmentMask()) >> s_.page_bits;
  }
  [[nodiscard]] constexpr LogAddress PageStart(const std::uint64_t page_number) const {
    return page_number << s_.page_bits;
  }

  // --- Getters ---
  [[nodiscard]] constexpr LogAddress GetBegin() const noexcept { return begin_; }
  [[nodiscard]] constexpr LogAddress GetTail() const noexcept { return tail_; }
  [[nodiscard]] constexpr LogAddress GetCommitted() const noexcept { return committed_; }
  [[nodiscard]] constexpr LogAddress GetHead() const noexcept { return head_; }

  void AdvanceBegin(const LogAddress addr) noexcept {
    if (addr > begin_) {
      begin_ = addr;
      // Head cannot be behind begin_
      if (head_ < begin_) head_ = begin_;
    }
  }

  // --- Mutators ---
  // The caller (FastLog) is responsible for ensuring there is enough capacity before
  // calling this!
  void AdvanceTail(const size_t bytes) { tail_ += bytes; }

  void AdvanceHead(const LogAddress new_head) { head_ = new_head; }
  void AdvanceCommitted(const LogAddress new_committed) { committed_ = new_committed; }

  // Allocates 'size' bytes in the log, automatically handling page boundary padding.
  // Returns the exact logical address where the caller should write their record.
  // The caller MUST ensure HasCapacityFor(size) is true before calling this.
  LogAddress Allocate(const std::size_t size) {
    const auto offset = OffsetInPageOf(tail_);

    if (const auto remaining_in_page = s_.PageSize() - offset; remaining_in_page < size) {
      // Pad to the next page
      AdvanceTail(remaining_in_page);
    }

    // This is the address where the record will actually start
    const LogAddress allocated_addr = tail_;

    // Advance the tail for the actual record
    AdvanceTail(size);

    return allocated_addr;
  }

  // --- Questions the FastLog can ask ---

  // Total memory capacity in bytes
  [[nodiscard]] constexpr uint64_t MemoryCapacity() const noexcept {
    return s_.MemorySize();
  }

  // How many bytes are currently taking up physical RAM?
  [[nodiscard]] constexpr uint64_t ResidentSize() const { return tail_ - head_; }

  // Does the circular buffer have enough physical room for this append,
  // accounting for any page-boundary padding if the record doesn't fit?
  [[nodiscard]] bool HasCapacityFor(const size_t bytes) const {
    size_t total_bytes = bytes;
    const auto offset = OffsetInPageOf(tail_);

    // If it doesn't fit in the current page, we must add the remaining
    // bytes to our capacity requirement because they will be wasted as padding.
    if (const auto remaining_in_page = s_.PageSize() - offset;
        remaining_in_page < bytes) {
      total_bytes += remaining_in_page;
    }

    return ResidentSize() + total_bytes <= MemoryCapacity();
  }

  // How much data is sitting in RAM waiting to be flushed?
  [[nodiscard]] constexpr uint64_t UnflushedBytes() const { return tail_ - committed_; }
};

/// Partition is a single threaded, fully independent Log
/// FastLog is a durable sequence of opaque records. Enqueue(blob) appends one record and
/// returns its address; the blob is stored and returned verbatim as a single unit. The
/// log frames, addresses, checksums, and recovers per record. It does not interpret a
/// record's contents and does not support sub-record or cross-record byte access.
class Partition {
  friend class PartitionTest;

  tinyio::IO& io_;
  FastLogSettings settings_;
  AddressManager am_;
  PageArena arena_;
  std::optional<tinyio::Fd> active_segment_;
  Cache<uint64_t, std::shared_ptr<tinyio::Fd>> ro_fd_cache_;
  LogAddress persisted_begin_ = 0;

  tinyio::Result<void> RotateSegment(LogAddress start);
  tinyio::Result<std::shared_ptr<tinyio::Fd>> GetReadSegmentFd(uint64_t segment_id);
  tinyio::Result<uint32_t> ReadFromRam(LogAddress addr, std::span<std::byte> buffer);
  tinyio::Result<uint32_t> ReadFromDisk(LogAddress addr, std::span<std::byte> buffer);
  std::optional<LogAddress> AppendRecord(RecordOp op, std::span<const std::byte> payload);

  tinyio::Result<void> EnsureDirectory() noexcept;

 public:
  /// Create a partition.
  /// Ensure the directory exists, if not create it.
  /// If the directory exist, get some data about it like the starting tail
  Partition(const FastLogSettings& settings, tinyio::IO& io);

  // while Enqueue is a byte slice, it should be seen as a singe recorde.
  // The reason is that it would be retrieved as it. There is no such thing like
  // partial retrieve.
  // Append is a memory only operation, If the arena cannot get new records,
  // the operation return a result error
  std::optional<LogAddress> Append(std::span<const std::byte> record) {
    return AppendRecord(RecordOp::Upsert, record);
  }

  std::optional<LogAddress> Delete(std::span<const std::byte> key_record) {
    return AppendRecord(RecordOp::Delete, key_record);
  }

  tinyio::Result<uint32_t> Read(LogAddress addr, std::span<std::byte> buffer);

  // Flush to disk, and fsync, use strong for datasync
  tinyio::Result<void> Commit(bool strong = false);

  /// Truncate the log until, but not including, untilAddress. **User should ensure
  /// that the provided address is a valid starting address for some record.** The
  /// truncation is only persisted upon the next successful commit.
  tinyio::Result<void> TruncateUntil(LogAddress addr);
};
}  // namespace fastlog