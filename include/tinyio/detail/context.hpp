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
    std::uint64_t rbx = 0;
    std::uint64_t rbp = 0;
    std::uint64_t r12 = 0;
    std::uint64_t r13 = 0;
    std::uint64_t r14 = 0;
    std::uint64_t r15 = 0;
};
static_assert(offsetof(Context, rsp) == 0);
static_assert(offsetof(Context, rip) == 8);
static_assert(offsetof(Context, rbx) == 16);
static_assert(offsetof(Context, rbp) == 24);
static_assert(offsetof(Context, r12) == 32);
static_assert(offsetof(Context, r13) == 40);
static_assert(offsetof(Context, r14) == 48);
static_assert(offsetof(Context, r15) == 56);
#elif defined(__aarch64__)
struct Context
{
    std::uint64_t sp = 0;
    std::uint64_t pc = 0;
    std::uint64_t x19 = 0;
    std::uint64_t x20 = 0;
    std::uint64_t x21 = 0;
    std::uint64_t x22 = 0;
    std::uint64_t x23 = 0;
    std::uint64_t x24 = 0;
    std::uint64_t x25 = 0;
    std::uint64_t x26 = 0;
    std::uint64_t x27 = 0;
    std::uint64_t x28 = 0;
    std::uint64_t x29 = 0;
    std::uint64_t x30 = 0;
    std::uint64_t d8 = 0;
    std::uint64_t d9 = 0;
    std::uint64_t d10 = 0;
    std::uint64_t d11 = 0;
    std::uint64_t d12 = 0;
    std::uint64_t d13 = 0;
    std::uint64_t d14 = 0;
    std::uint64_t d15 = 0;
};
static_assert(offsetof(Context, sp) == 0);
static_assert(offsetof(Context, pc) == 8);
static_assert(offsetof(Context, x19) == 16);
static_assert(offsetof(Context, x20) == 24);
static_assert(offsetof(Context, x21) == 32);
static_assert(offsetof(Context, x22) == 40);
static_assert(offsetof(Context, x23) == 48);
static_assert(offsetof(Context, x24) == 56);
static_assert(offsetof(Context, x25) == 64);
static_assert(offsetof(Context, x26) == 72);
static_assert(offsetof(Context, x27) == 80);
static_assert(offsetof(Context, x28) == 88);
static_assert(offsetof(Context, x29) == 96);
static_assert(offsetof(Context, x30) == 104);
static_assert(offsetof(Context, d8) == 112);
static_assert(offsetof(Context, d15) == 168);
#else
    #error "tinyio supports x86_64 and aarch64 only"
#endif

}  // namespace tinyio::detail::context

extern "C" void tinyio_ctx_switch(tinyio::detail::context::Context* from,
                                  tinyio::detail::context::Context* to) noexcept;
extern "C" void tinyio_fiber_start() noexcept;
extern "C" [[noreturn]] void tinyio_fiber_entry(void* fiber) noexcept;
