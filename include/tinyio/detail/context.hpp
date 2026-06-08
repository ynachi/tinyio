#pragma once

#include <cstddef>
#include <cstdint>

namespace tinyio::detail::context
{

#if defined(__x86_64__)
struct Context
{
    std::uint64_t rsp = 0;
    std::uint64_t rip = 0;
};
static_assert(offsetof(Context, rsp) == 0);
static_assert(offsetof(Context, rip) == 8);

#elif defined(__aarch64__)
struct Context
{
    std::uint64_t sp = 0;
    std::uint64_t pc = 0;
};
static_assert(offsetof(Context, sp) == 0);
static_assert(offsetof(Context, pc) == 8);
#else
    #error "tinyio supports x86_64 and aarch64 only"
#endif

}  // namespace tinyio::detail::context

// preserve_none is a Clang-only calling convention attribute (as of 2025).
// With it, the compiler saves only live registers at each context-switch call site.
#if defined(__clang__)
#define TIO_PRESERVE_NONE __attribute__((preserve_none))
#else
#define TIO_PRESERVE_NONE
#endif

extern "C" TIO_PRESERVE_NONE void tinyio_ctx_switch(tinyio::detail::context::Context* from,
                                  tinyio::detail::context::Context* to) noexcept;

extern "C" void tinyio_fiber_start() noexcept;
extern "C" [[noreturn]] void tinyio_fiber_entry(void* fiber) noexcept;
