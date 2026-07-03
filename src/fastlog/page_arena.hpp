#pragma once
#include <cstddef>
#include <cstring>
#include <span>

#include "tinyio/tinyio.hpp"

namespace fastlog {
class PageArena {
  std::size_t page_size_;
  tinyio::FixedBuffer buffer_;

 public:
  PageArena(tinyio::FixedBufferPool& pool, std::size_t page_size, std::size_t num_pages);

  // Dev note: no bound checking, bound checking will be performed once on the public API
  std::span<const std::byte> Read(const std::size_t offset,
                                  const std::size_t length) const {
    return buffer_.Data().subspan(offset, length);
  }

  void Write(const std::size_t offset, std::span<const std::byte> data) {
    std::memcpy(buffer_.Ptr() + offset, data.data(), data.size());
  }

  tinyio::FixedBuffer& Buffer() { return buffer_; }
  const tinyio::FixedBuffer& Buffer() const { return buffer_; }

  void ClearSlot(const std::size_t slot) {
    std::memset(buffer_.Ptr() + slot * page_size_, 0, page_size_);
  }
};
}  // namespace fastlog