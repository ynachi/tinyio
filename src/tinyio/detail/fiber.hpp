#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

#include "context.hpp"
#include "sanitizer_hooks.hpp"

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
enum class WaitKind : std::uint8_t { none, io, sync };

struct Fiber;

/// \brief State for a single asynchronous I/O operation.
///
/// A pointer to this struct is passed to `io_uring_sqe_set_data()`. When the kernel
/// finishes the operation, the scheduler reads this pointer from the Completion Queue
/// Entry (CQE) to wake up the associated fiber.
struct Operation {
  /// Pointer back to the fiber that initiated this operation.
  Fiber* fiber = nullptr;

  /// Static context string (e.g., "read", "accept") used for debugging and errors.
  const char* op_name = "";

  /// The return value provided by the kernel upon completion (equivalent to errno/bytes
  /// read).
  int cqe_res = 0;

  /// The completion flags provided by the kernel.
  unsigned cqe_flags = 0;

  /// Absolute CLOCK_MONOTONIC deadline (ns) while this op is armed in the
  /// runtime's TimerWheel; 0 when no timeout is attached.
  uint64_t deadline_ns = 0;

  /// Set by the timer wheel when it expires this op (before issuing the
  /// ASYNC_CANCEL). Lets syscall_timed() map the resulting -ECANCELED to
  /// ETIMEDOUT instead of ECANCELED.
  bool timed_out = false;
};

/// \brief Represents a single cooperative thread of execution (coroutine).
struct Fiber {
  /// Pointer to the main event loop / scheduler that owns this fiber.
  IO* runtime = nullptr;

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
  std::move_only_function<void(IO&) noexcept> task;

  /// The pre-allocated I/O operation data. By embedding this inside the fiber,
  /// we avoid heap allocations for every single read/write operation.
  ///
  /// Invariant: only one operation may be in flight per fiber at a time.
  /// This is enforced structurally — the fiber is suspended (state == waiting)
  /// for the entire duration of an operation, so it cannot submit a second one
  /// until the first CQE is received and the fiber is resumed. The assertion
  /// `state == FiberState::running` in IO::syscall() makes this explicit.
  Operation operation{};

  /// The current lifecycle status of the fiber.
  FiberState state = FiberState::ready;

  /// Why the fiber is suspended.
  WaitKind wait_kind = WaitKind::none;

  /// True while the fiber sits in the runtime's ready queue. Stops PushReady()
  /// from queueing the same fiber twice.
  bool in_ready_queue = false;

  /// True while the fiber is armed in the TimerWheel.
  bool timer_armed = false;

  /// Position in its TimerWheel slot while timer_armed (O(1) disarm).
  std::size_t timer_index = 0;

  /// Position in the runtime's all_fibers_ vector (O(1) removal).
  std::size_t all_fibers_index = 0;
};
}  // namespace tinyio::detail::fiber
