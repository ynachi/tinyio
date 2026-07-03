#include "page_arena.hpp"

namespace fastlog {
PageArena::PageArena(tinyio::FixedBufferPool& pool, const std::size_t page_size,
                     const std::size_t num_pages)
    : page_size_(page_size), buffer_([&] {
        auto res = pool.Take(page_size * num_pages);
        if (!res.has_value()) {
          throw std::runtime_error("Failed to allocate page arena: " +
                                   res.error().Message());
        }
        return std::move(res).value();
      }()) {}
}  // namespace fastlog
