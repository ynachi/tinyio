#pragma once
#include <fcntl.h>

#include <filesystem>

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
namespace tinyio {

using SecNo = uint64_t;
using SegmentID = uint64_t;

// TODO: split read and write options ?
struct WalOptions {
    std::filesystem::path directory;

    size_t max_tasks_per_io_engine = 16800;

    mode_t file_mode = 0644;
    mode_t dir_mode = 0755;
    int read_flags = O_RDONLY;
    int write_flags = O_CREAT | O_RDWR;

    // File rotation
    // TODO: should wal be aware of this ?
    size_t max_file_size = 100 * 1024 * 1024;  // 100MB
};
}  // namespace tinyio
