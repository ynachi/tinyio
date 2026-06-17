#pragma once

#include "tinyio/fd.hpp"

#include <memory>

#include "cache.hpp"
#include "types.hpp"

namespace bitcask
{
/// Fdcache using a shared ptr of FD. On put, the cache would return
/// the evicted Fd. We advise the caller to check and close the Fd
/// asynchronously if they are the sole owner.
using FdCache = Cache<SegmentId, std::shared_ptr<tinyio::Fd>>;
}  // namespace bitcask