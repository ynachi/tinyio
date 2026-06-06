#pragma once
#include "uring/core/io.h"

#include <cassert>
#include <concepts>
#include <thread>

#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#include "buffer_pool.hpp"
#include "detail/fiber.hpp"
#include "error.hpp"
#include <bits/move_only_function.h>

namespace tinyio
{

/// Default io uring flag
static constexpr unsigned kUringDefaultAdditionalFlag =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;

/// These are mandatory base flags
static constexpr unsigned kUringBaseFlags = IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP | IORING_SETUP_R_DISABLED;

/// Default io accept flags
static constexpr int kDefaultAcceptFlags = SOCK_NONBLOCK | SOCK_CLOEXEC;

struct RuntimeOptions
{
    /// Additional flags passed to io_uring
    unsigned additional_uring_flags = kUringDefaultAdditionalFlag;

    /// Max tick timeout
    std::uint32_t tick_timeout_ms = 10;

    /// Number of submission queue entries requested from io_uring.
    std::uint32_t sq_entries = 4096;

    /// Number of completion queue entries requested from io_uring.
    std::uint32_t cq_entries = 8192;

    /// Default usable stack size for newly spawned fibers.
    std::size_t stack_size = 256 * 1024;

    /// Guard bytes placed below each fiber stack with PROT_NONE.
    std::size_t guard_size = 4096;

    /// Enables IORING_SETUP_CLAMP when available in the kernel headers.
    bool clamp_ring_sizes = true;

    /// max resume per tick
    std::size_t batch_max_size = 128;

    // Liburing auto wakeup the kernel thread so no need to manually do it
    /// IORING_SETUP_DEFER_TASKRUN is not compatible to SQ_POLL
    /// Also, when SQ_POLL is enabled, make sure to pin work threads and kernel threads
    /// and do them on different CPUs, overwhise, the bench reveals that performance
    /// drops on throughput and latency.
    std::uint32_t sq_thread_idle_ms = 2000;
    // -1 means don't pin to a specific CPU
    int sq_thread_cpu = -1;

    /// Total number of fibers which can be recycle
    std::size_t max_idle_fibers = 1024;
};

class Runtime final
{
public:
    /// Constructs an uninitialized Runtime shell; create() performs actual initialization.
    explicit Runtime(RuntimeOptions options) noexcept;

    /// Destroys the runtime and releases ring resources and all fiber stacks.
    ///
    /// Production code should call run() until all spawned fibers complete before destruction.
    ~Runtime() noexcept;

    /// Runtime is not copyable because it uniquely owns fibers and an io_uring instance.
    Runtime(const Runtime&) = delete;

    /// Runtime is not copy-assignable because ownership is unique and stable.
    Runtime& operator=(const Runtime&) = delete;

    /// Runtime is not movable because Fiber objects store their owning Runtime address.
    Runtime(Runtime&&) = delete;

    /// Runtime is not move-assignable because address stability is part of the design.
    Runtime& operator=(Runtime&&) = delete;

    /// Spawns one stackful fiber owned by this runtime.
    ///
    /// The callable is invoked as fn(IO&). The fiber never migrates to another Runtime.
    /// This method is owner-thread-only. It catches construction/allocation failures and returns Result.
    template <class Fn>
        requires std::constructible_from<std::move_only_function<void(Runtime&) noexcept>, Fn>
    [[nodiscard]] Result<FiberId> spawn(Fn&& fn) noexcept
    {
        try
        {
            return spawn_task(std::move_only_function<void(Runtime&) noexcept>(std::forward<Fn>(fn)));
        }
        catch (const std::bad_alloc&)
        {
            return fail(ENOMEM, "spawn");
        }
        catch (...)
        {
            return fail(EINVAL, "spawn");
        }
    }

    /// Runs the cooperative scheduler until all fibers finish or stop is requested.
    ///
    /// This method is owner-thread-only. It blocks the owner thread only when no fiber is ready and
    /// the runtime is waiting for io_uring completions.
    [[nodiscard]] Result<void> run() noexcept;

    /// Suspend the current fiber to give bck the control to the runtime
    [[nodiscard]] Result<void> yield() noexcept;

    /// Requests the run loop to stop after the current scheduler turn.
    ///
    /// This is owner-thread-only in v1. Cross-runtime wakeup must be explicit in a later API.
    void request_stop() noexcept;

    /// Returns true when there are live fibers still owned by this runtime.
    [[nodiscard]] bool has_live_fibers() const noexcept;

    /// Checks if we are running an operation on the thread which created this IO instance
    [[nodiscard]] bool is_owner_thread() const noexcept { return std::this_thread::get_id() == owner_tid_; }

    /// Register a fixed buffer pool
    Result<void> register_buffers(FixedBufferPool& pool) noexcept;
    [[nodiscard]] Result<FixedBuffer> take_fixed_buffer(const size_t size) noexcept { return buffer_pool_.take(size); }

private:
    //
    // IO Uring management
    //
    io_uring ring_{};
    Result<void> submit_or_wait_for() noexcept;
    Result<io_uring_sqe*> get_sqe() noexcept;

    // Internal executor for I/O operations. Factorizes the SQE acquisition,
    // fiber suspension, and error mapping logic.
    template <typename T = uint32_t, typename Prep>
    Result<T> syscall(const char* op_name, Prep&& prep) noexcept
    {
        // - Prevents multi-threaded corruption of io_uring
        // - Prevents calling io.read() from outside a fiber
        if (!is_owner_thread() || current_fiber_ == nullptr)
        {
            return fail(EPERM, op_name);
        }

        // Internal Sanity Checks
        assert(current_fiber_->runtime == this);
        assert(current_fiber_->state == detail::fiber::FiberState::running);

        auto sqe = TRY_LOG_ERR(get_sqe(), "failed to get sqe: io ops [{}]", op_name);

        detail::fiber::Operation& op = current_fiber_->operation;
        op = detail::fiber::Operation{};
        op.fiber = current_fiber_;
        op.op_name = op_name;

        std::forward<Prep>(prep)(sqe);
        io_uring_sqe_set_data64(sqe, reinterpret_cast<__u64>(&op));

        current_fiber_->state = detail::fiber::FiberState::waiting;
        ++inflight_ops_;

        // suspend now
        suspend_fiber(current_fiber_);

        if (op.cqe_res < 0)
        {
            return fail(-op.cqe_res, op_name);
        }

        if constexpr (std::is_same_v<T, void>)
        {
            return {};
        }
        else
        {
            return static_cast<T>(op.cqe_res);
        }
    }
    
    //
    // event fd for cross-thread signaling
    //
    int wake_fd_{-1};
    uint64_t wake_value_{0};

    //
    // Lifecycle
    //
    bool is_activated_{false};
    bool is_running_{false};
    // cross-thread needs to check this, this is why it is an atomic
    alignas(64) std::atomic<bool> is_sleeping_{false};
    std::thread::id owner_tid_;

    //
    // Configs
    //
    std::thread::id thread_id_;
    std::size_t id_;  ///  Id of the worker in a multi-threaded context
    RuntimeOptions opts_;

    FixedBufferPool buffer_pool_{};

    //
    // Fiber management
    //
    bool inside_run_ = false;
    detail::context::Context scheduler_context_{};
    detail::ToolFiberState scheduler_tools_{};
    IntrusiveList<detail::fiber::Fiber, AllFibersTag> all_fibers_;
    IntrusiveList<detail::fiber::Fiber, ReadyQueueTag> ready_queue_;
    IntrusiveList<detail::fiber::Fiber, ReadyQueueTag> idle_fibers_;  ///< For recycle
    detail::fiber::Fiber* current_fiber_ = nullptr;
    FiberId next_fiber_id_ = 1;
    std::size_t live_fibers_ = 0;
    std::size_t inflight_ops_ = 0;
    // std::size_t pending_submissions_ = 0;

    void push_ready(detail::fiber::Fiber* fiber) noexcept;
    void remove_ready(detail::fiber::Fiber* fiber) noexcept;
    [[nodiscard]] detail::fiber::Fiber* pop_ready() noexcept;
    void destroy_fiber(detail::fiber::Fiber* fiber) noexcept;
    void destroy_all_fibers() noexcept;
    void suspend_fiber(detail::fiber::Fiber* fiber) noexcept;
    void resume_fiber(detail::fiber::Fiber* fiber) noexcept;

    //
    // Other private helper methods
    //

    /// Initializes the io_uring instance and validates required opcodes.
    [[nodiscard]] Result<void> initialize() noexcept;

    /// Spawns a fiber from an already-erased task object.
    [[nodiscard]] Result<FiberId> spawn_task(std::move_only_function<void(Runtime&) noexcept> task) noexcept;
};
}  // namespace tinyio