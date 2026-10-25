#pragma once
#include <cstddef>
#include <cstdint>

#include "tinyio/detail/context.hpp"
#include "tinyio/detail/sanitizer_hooks.hpp"

namespace tinyio {
class IO;

using FiberId = std::uint64_t;
}  // namespace tinyio

namespace tinyio::detail::fiber {
/// \brief Represents the execution lifecycle of a cooperative fiber.
enum class FiberState : std::uint8_t {
    /// The fiber is in the ready queue, waiting for the scheduler to give it CPU time.
    ready,

    /// The fiber is currently actively executing on the OS thread.
    running,

    /// The fiber is suspended, waiting for an io_uring completion event from the kernel.
    waiting,

    /// The fiber's task function has returned and it is queued for cleanup/destruction.
    done,
};

/// \brief Indicates why a fiber is suspended.
///
/// A fiber that parks on a synchronization primitive records WHICH primitive
/// it is waiting for, so a runtime that quiesces with parked fibers can name
/// the culprit instead of printing a count. There is no counter to maintain:
/// the reason lives in the fiber that is parked, which is the one place that
/// cannot lose it.
enum class WaitKind : std::uint8_t {
    none,
    /// Parked on an io_uring completion.
    io,
    /// Parked on a Mutex.
    mutex,
    /// Parked on a Semaphore.
    semaphore,
    /// Parked on a Latch.
    latch,
    /// Parked on a TaskGroup.
    task_group,
    /// Parked waiting for a fixed buffer slot.
    buffer_pool,
};

/// @brief The human-readable name of a wait reason, for diagnostics.
[[nodiscard]] constexpr const char* WaitKindName(const WaitKind kind) noexcept {
    switch (kind) {
        case WaitKind::none:
            return "none";
        case WaitKind::io:
            return "I/O";
        case WaitKind::mutex:
            return "Mutex";
        case WaitKind::semaphore:
            return "Semaphore";
        case WaitKind::latch:
            return "Latch";
        case WaitKind::task_group:
            return "TaskGroup";
        case WaitKind::buffer_pool:
            return "FixedBufferPool";
    }
    return "unknown";
}

struct Fiber;

/// \brief State for a single asynchronous I/O operation.
///
/// A pointer to this struct is passed to `io_uring_sqe_set_data()`. When the kernel
/// finishes the operation, the scheduler reads this pointer from the Completion Queue
/// Entry (CQE) to wake up the associated fiber.
struct Operation {
    /// Pointer back to the fiber that initiated this operation.
    Fiber* fiber = nullptr;

    /// Absolute CLOCK_MONOTONIC deadline (ns) while this op is armed in the
    /// runtime's TimerWheel; 0 when no timeout is attached.
    uint64_t deadline_ns = 0;

    /// The kernel's result, copied from the CQE: a negative errno on failure,
    /// otherwise the operation's result (bytes transferred, a new fd, or 0).
    /// IO::Syscall() turns it into a Result once the fiber resumes.
    int cqe_res = 0;

    /// Set by the timer wheel when it expires this op (before issuing the
    /// ASYNC_CANCEL). Lets Syscall() map the resulting -ECANCELED to
    /// ETIMEDOUT instead of ECANCELED.
    bool timed_out = false;
};

/// \brief Represents a single cooperative thread of execution (fiber).
///
/// Fields are ordered so the hot CQE-drain and context-switch path
/// (context, runtime, state, wait_kind, timer_armed, operation) fits inside
/// a single 64-byte cache line in release builds.
struct alignas(64) Fiber {
    /// Holds the CPU register state (Stack Pointer) when the fiber is suspended.
    context::Context context{};

    /// Pointer to the main event loop / scheduler that owns this fiber.
    IO* runtime = nullptr;

    /// The current lifecycle status of the fiber. `ready` means exactly "in the
    /// runtime's ready queue": PushReady() relies on it to queue a fiber once.
    FiberState state = FiberState::ready;

    /// Why the fiber is suspended.
    WaitKind wait_kind = WaitKind::none;

    /// True while the fiber is armed in the TimerWheel.
    bool timer_armed = false;

    /// The pre-allocated I/O operation data. By embedding this inside the fiber,
    /// we avoid heap allocations for every single read/write operation.
    ///
    /// Invariant: only one operation may be in flight per fiber at a time.
    /// This is enforced structurally — the fiber is suspended (state == waiting)
    /// for the entire duration of an operation, so it cannot submit a second one
    /// until the first CQE is received and the fiber is resumed. The assertion
    /// `state == FiberState::running` in IO::Syscall() makes this explicit.
    Operation operation{};

    /// Integration state for Address Sanitizer (ASAN) and Thread Sanitizer (TSAN).
    [[no_unique_address]] ToolFiberState tools{};

    /// The allocated memory region for the fiber's local execution stack.
    stack::Stack stack{};

    /// Position in its TimerWheel slot while timer_armed (O(1) disarm).
    std::size_t timer_index = 0;

    /// Position in the runtime's all_fibers_ vector (O(1) removal).
    std::size_t all_fibers_index = 0;

    /// A unique, monotonically increasing identifier for debugging.
    FiberId id = 0;

    /// Pointer to the callable constructed at the top of this fiber's stack.
    void* task_ptr = nullptr;

    /// Type-erased destructor for the stack-allocated callable; cleared once run.
    void (*destroy_task)(void*) noexcept = nullptr;

    void DestroyTask() noexcept {
        if (destroy_task != nullptr) {
            auto fn = destroy_task;
            void* ptr = task_ptr;
            destroy_task = nullptr;
            task_ptr = nullptr;
            fn(ptr);
        }
    }
};
}  // namespace tinyio::detail::fiber
