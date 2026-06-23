#pragma once

#include <cstddef>
#include <cstdint>

#include "tinyio/error.hpp"

namespace tinyio::detail::stack {

struct Stack {
  void* mapping = nullptr;
  std::size_t mapping_size = 0;
  std::byte* low = nullptr;
  std::byte* high = nullptr;
  std::size_t usable_size = 0;
  unsigned valgrind_id = 0;
};

[[nodiscard]] Result<Stack> AllocateStack(std::size_t usable_size,
                                          std::size_t guard_size) noexcept;
void ReleaseStack(Stack& stack) noexcept;

}  // namespace tinyio::detail::stack
