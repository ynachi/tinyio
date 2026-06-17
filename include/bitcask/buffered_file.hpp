#pragma once

#include "tinyio/tinyio.hpp"

#include <optional>

namespace tinyio::lsm
{
class BufferedFile
{
    // holds a 4k aligned fixed buffer pool
    IO& io_;

    std::optional<FixedBuffer> active_buf_ = std::nullopt;
    std::uint64_t wr_off_ = 0;
};
}  // namespace tinyio::lsm