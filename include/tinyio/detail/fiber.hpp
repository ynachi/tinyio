#pragma once
#include <cstdint>

#include <linux/time_types.h>

#include "context.hpp"
#include "sanitizer_hooks.hpp"
#include "tinyio/intrusive_list.hpp"
#include <bits/move_only_function.h>

namespace tinyio
{
class Runtime;

using FiberId = std::uint64_t;
}  // namespace tinyio

namespace tinyio::detail::fiber
{
/// \brief Represents the execution lifecycle of a cooperative fiber.
enum class FiberState : std::uint8_t
{
    /// The fiber is in the ready queue, waiting for the scheduler to give it CPU time.
    ready,

    /// The fiber is currently actively executing on the OS thread.
    running,

    /// The fiber is suspended, waiting for an io_uring completion event from the kernel.
    waiting,

    /// The fiber's task function has returned and it is queued for cleanup/destruction.
    done,
};

struct Fiber;

/// \brief State for a single asynchronous I/O operation.
///
/// A pointer to this struct is passed to `io_uring_sqe_set_data()`. When the kernel
/// finishes the operation, the scheduler reads this pointer from the Completion Queue
/// Entry (CQE) to wake up the associated fiber.
struct Operation
{
    /// Pointer back to the fiber that initiated this operation.
    Fiber* fiber = nullptr;

    /// Static context string (e.g., "read", "accept") used for debugging and errors.
    const char* op_name = "";

    /// The return value provided by the kernel upon completion (equivalent to errno/bytes read).
    int cqe_res = 0;

    /// The completion flags provided by the kernel.
    unsigned cqe_flags = 0;

    /// Persists timeout structs in user-space. io_uring requires this struct to
    /// remain valid in memory until the timeout operation completes in the kernel.
    __kernel_timespec timeout_ts{};
};

/// \brief Represents a single cooperative thread of execution (coroutine).
/// Intrusive doubly-linked list
struct Fiber : IntrusiveHook<AllFibersTag>, IntrusiveHook<ReadyQueueTag>
{
    /// Pointer to the main event loop / scheduler that owns this fiber.
    Runtime* runtime = nullptr;

    /// A unique, monotonically increasing identifier for debugging.
    FiberId id = 0;

    /// Holds the CPU register state (Stack Pointer, Instruction Pointer, etc.)
    /// when the fiber is suspended.
    context::Context context{};

    /// The allocated memory region for the fiber's local execution stack.
    stack::Stack stack{};

    /// Integration state for Address Sanitizer (ASAN) and Thread Sanitizer (TSAN).
    ToolFiberState tools{};

    /// The actual user-provided function or lambda that this fiber executes.
    std::move_only_function<void(Runtime&) noexcept> task;

    /// The pre-allocated I/O operation data. By embedding this inside the fiber,
    /// we avoid heap allocations for every single read/write operation.
    Operation operation{};

    /// The current lifecycle status of the fiber.
    FiberState state = FiberState::ready;
};
}  // namespace tinyio::detail::fiber