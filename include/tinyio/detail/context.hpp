#pragma once

#include <cstddef>
#include <cstdint>

namespace tinyio::detail::context
{

struct Context
{
    std::uintptr_t sp = 0;
};
static_assert(offsetof(Context, sp) == 0);

}  // namespace tinyio::detail::context

extern "C" void tinyio_ctx_switch(tinyio::detail::context::Context* from,
                                  tinyio::detail::context::Context* to) noexcept;

extern "C" void tinyio_fiber_start() noexcept;
extern "C" [[noreturn]] void tinyio_fiber_entry(void* fiber) noexcept;
