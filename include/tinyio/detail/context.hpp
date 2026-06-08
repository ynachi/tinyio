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

// preserve_none: the compiler saves only live registers at each call site.
// Supported by Clang (all versions with this feature) and GCC >=15 (x86-64) / GCC >=16 (aarch64).
// Both compilers pass the first two integer args in:
//   x86-64:  %r12, %r13
//   aarch64: x20, x21
// and treat all other GPRs (except rsp/rbp or sp/x29/x30) as caller-saved.
#if defined(__clang__)
    // Clang has supported preserve_none since Clang 18 (x86-64) and Clang 19 (aarch64)
    #define TIO_PRESERVE_NONE __attribute__((preserve_none))
#elif defined(__GNUC__)
    #if defined(__x86_64__) && (__GNUC__ < 15)
        #error "tinyio requires GCC >= 15 for preserve_none on x86-64"
    #elif defined(__aarch64__) && (__GNUC__ < 16)
        #error "tinyio requires GCC >= 16 for preserve_none on aarch64"
    #endif
    #define TIO_PRESERVE_NONE __attribute__((preserve_none))
#else
    #error "tinyio requires Clang or GCC with preserve_none support"
#endif

extern "C" TIO_PRESERVE_NONE void tinyio_ctx_switch(tinyio::detail::context::Context* from,
                                  tinyio::detail::context::Context* to) noexcept;

extern "C" void tinyio_fiber_start() noexcept;
extern "C" [[noreturn]] void tinyio_fiber_entry(void* fiber) noexcept;
