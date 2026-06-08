#pragma once

#include <cstddef>

#include "stack.hpp"

#if defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define TINYIO_ASAN_ENABLED 1
    #endif
    #if __has_feature(thread_sanitizer)
        #define TINYIO_TSAN_ENABLED 1
    #endif
#endif

#if defined(__SANITIZE_ADDRESS__)
    #define TINYIO_ASAN_ENABLED 1
#endif

#ifndef TINYIO_ASAN_ENABLED
    #define TINYIO_ASAN_ENABLED 0
#endif
#ifndef TINYIO_TSAN_ENABLED
    #define TINYIO_TSAN_ENABLED 0
#endif

// Prevents ASAN from creating fake-stack frames for a function.
// Required on every function that calls tinyio_ctx_switch, because ASAN's
// fake-stack mechanism conflicts with hand-rolled stack switches.
#if defined(__clang__) || defined(__GNUC__)
    #define TIO_NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))
#else
    #define TIO_NO_SANITIZE_ADDRESS
#endif

#if TINYIO_ASAN_ENABLED
extern "C" void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
extern "C" void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, std::size_t* size_old);
#endif

#if TINYIO_TSAN_ENABLED
extern "C" void* __tsan_get_current_fiber(void);
extern "C" void* __tsan_create_fiber(unsigned flags);
extern "C" void __tsan_destroy_fiber(void* fiber);
extern "C" void __tsan_switch_to_fiber(void* fiber, unsigned flags);
#endif

namespace tinyio::detail
{

struct ToolFiberState
{
    void* asan_fake_stack = nullptr;
    void* tsan_fiber = nullptr;
    bool owns_tsan_fiber = false;
};

inline void tools_capture_current_thread(ToolFiberState& state) noexcept
{
#if TINYIO_TSAN_ENABLED
    state.tsan_fiber = __tsan_get_current_fiber();
    state.owns_tsan_fiber = false;
#else
    (void)state;
#endif
}

inline void tools_create_fiber(ToolFiberState& state) noexcept
{
#if TINYIO_ASAN_ENABLED
    state.asan_fake_stack = nullptr;
#endif
#if TINYIO_TSAN_ENABLED
    state.tsan_fiber = __tsan_create_fiber(0);
    state.owns_tsan_fiber = true;
#else
    (void)state;
#endif
}

inline void tools_destroy_fiber(ToolFiberState& state) noexcept
{
#if TINYIO_TSAN_ENABLED
    if (state.tsan_fiber != nullptr && state.owns_tsan_fiber)
    {
        __tsan_destroy_fiber(state.tsan_fiber);
    }
    state.tsan_fiber = nullptr;
    state.owns_tsan_fiber = false;
#else
    (void)state;
#endif
}

inline void tools_before_switch(ToolFiberState& from, ToolFiberState* to_tools, const stack::Stack* to_stack) noexcept
{
#if TINYIO_ASAN_ENABLED
    const void* bottom = to_stack == nullptr ? nullptr : to_stack->low;
    const std::size_t size = to_stack == nullptr ? 0U : to_stack->usable_size;
    __sanitizer_start_switch_fiber(&from.asan_fake_stack, bottom, size);
#else
    (void)from;
    (void)to_stack;
#endif

#if TINYIO_TSAN_ENABLED
    if (to_tools != nullptr && to_tools->tsan_fiber != nullptr)
    {
        __tsan_switch_to_fiber(to_tools->tsan_fiber, 0);
    }
#else
    (void)to_tools;
#endif
}

inline void tools_after_switch(ToolFiberState& current) noexcept
{
#if TINYIO_ASAN_ENABLED
    __sanitizer_finish_switch_fiber(current.asan_fake_stack, nullptr, nullptr);
#else
    (void)current;
#endif
}

}  // namespace tinyio::detail
