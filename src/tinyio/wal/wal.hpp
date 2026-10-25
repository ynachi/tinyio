#pragma once
#include <fcntl.h>

#include <cstddef>
#include <filesystem>
#include <span>

/*
Segment file header (64 B, written once):
+--------+---------+--------------+--------+------------+
| magic  | version | first_seq    | salt   | header_crc |
| 4 B    | 2 B     | 8 B          | 8 B    | 4 B        |  (+pad)
+--------+---------+--------------+--------+------------+

record
*+---------+--------+--------+-----------+-----------+
| length  |  seq   |  salt  |  payload  |    crc    |
|  4 B    |  8 B   |  8 B   | length B  |   4 B     |
+---------+--------+--------+-----------+-----------+

*/
namespace tinyio::wal {

inline constexpr uint32_t kSegmentMagic = 0x54494F57;  // "WOIT"

using Lsn = uint64_t;
using SegmentID = uint64_t;
using Record = std::span<const std::byte>;

/// Segment naming and discovery, shared by Writer and Reader.
[[nodiscard]] std::filesystem::path SegmentPath(const std::filesystem::path& dir, SegmentID id);

struct LsnRange {
    Lsn start;
    uint32_t count;
    constexpr Lsn Start() const { return start; }
    Lsn End() const { return start + count - 1; }
    constexpr LsnRange(Lsn start_lsn, uint32_t record_count)
        : start(start_lsn), count(record_count) {}
};

// TODO: split read and write options ?
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
}  // namespace tinyio::wal
