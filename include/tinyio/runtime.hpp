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
#include "fd.hpp"
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

    // =====
    // IO Methods
    // =====
    /// Write up to @p len bytes from @p buf (a registered fixed buffer) to @p fd
    /// starting at @p offset.
    ///
    /// Uses io_uring's WRITE_FIXED operation, which avoids the kernel copy into
    /// a pinned buffer because @p buf is already registered with the ring.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Number of bytes written, or an error code.
    ///
    /// @code
    /// FIBER_TRY(auto buf, fio.take_fixed_buffer(4096));
    /// std::memcpy(buf.ptr(), data.data(), data.size());
    /// FIBER_TRY(auto n, fio.write_fixed(fd, buf, data.size(), file_offset));
    /// @endcode
    [[nodiscard]] Result<uint32_t> write_fixed(Fd& fd, const FixedBuffer& buf, size_t len, off_t offset = -1);

    /// Write the whole contents of @p buf to @p fd starting at @p offset.
    /// Equivalent to write_fixed(fd, buf, buf.size(), offset).
    [[nodiscard]] Result<uint32_t> write_fixed(Fd& fd, const FixedBuffer& buf, off_t offset = -1);
    /// Read up to @p len bytes from @p fd into @p buf (a registered fixed buffer)
    /// starting at @p offset.
    ///
    /// Uses io_uring's READ_FIXED operation.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Number of bytes read, or an error code.
    ///
    /// @code
    /// FIBER_TRY(auto buf, fio.take_fixed_buffer(4096));
    /// FIBER_TRY(auto n, fio.read_fixed(fd, buf, 4096, 0));
    /// std::string_view content{reinterpret_cast<const char*>(buf.ptr()),
    ///                          static_cast<size_t>(n)};
    /// @endcode
    [[nodiscard]] Result<uint32_t> read_fixed(Fd& fd, FixedBuffer& buf, size_t len, off_t offset = -1) noexcept;

    /// Read up to buf.size() bytes from @p fd into @p buf starting at @p offset.
    /// Equivalent to read_fixed(fd, buf, buf.size(), offset).
    [[nodiscard]] Result<uint32_t> read_fixed(Fd& fd, FixedBuffer& buf, off_t offset = -1) noexcept;

    /// Read up to buf.size() bytes from @p fd into the caller-provided span @p buf.
    ///
    /// Unlike read_fixed, this works with any user buffer but requires the kernel
    /// to copy data through an intermediate bounce buffer.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Number of bytes read, or an error code.
    [[nodiscard]] Result<uint32_t> read(Fd& fd, std::span<std::byte> buf, off_t offset = -1) noexcept;

    /// Write the scatter-gather vector @p iovecs to @p fd starting at @p offset.
    ///
    /// Wraps pwritev2 via io_uring's WRITEV operation.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Total bytes written, or an error code.
    [[nodiscard]] Result<uint32_t> writev(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1) noexcept;

    /// Flush pending writes on @p fd to the storage device.
    ///
    /// @param datasync  If true, flushes data only (fdatasync semantics —
    ///                  metadata such as mtime may lag).  If false, flushes
    ///                  both data and metadata (fsync semantics).
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void> fsync(Fd& fd, bool datasync = false) noexcept;

    /// Open or create the file at @p path and return a file descriptor.
    ///
    /// @param path
    /// @param flags  Standard open(2) flags such as O_RDONLY, O_CREAT | O_WRONLY.
    /// @param mode   Permission bits used when O_CREAT creates a new file.
    ///               Ignored if O_CREAT is not set.
    /// @return An owned Fd on success, or an error code (e.g. ENOENT, EACCES).
    ///
    /// @code
    /// FIBER_TRY(auto fd, fio.open("/var/log/app.log",
    ///                              O_CREAT | O_WRONLY | O_APPEND, 0644));
    /// @endcode
    [[nodiscard]] Result<Fd> open(const std::filesystem::path& path, int flags, mode_t mode = 0644) noexcept;

    /// Accept a new connection on @p server_fd, retrieving the client address.
    ///
    /// @return An owned Fd on success, or an error code.
    [[nodiscard]] Result<Fd> accept(Fd& server_fd, SocketAddress& client_addr, int flags = kDefaultAcceptFlags) noexcept;

    /// Accept a new connection on @p server_fd without retrieving the client address.
    ///
    /// @return An owned Fd on success, or an error code.
    [[nodiscard]] Result<Fd> accept(Fd& server_fd, int flags = kDefaultAcceptFlags) noexcept;

    /// Close @p fd asynchronously via io_uring.
    ///
    /// Takes ownership of @p fd so that it is released exactly once even if the
    /// close CQE returns an error.  Callers should use std::move:
    ///
    /// @code
    /// FIBER_TRY_VOID(fio.close(std::move(fd)));
    /// @endcode
    [[nodiscard]] Result<void> close(Fd&& fd) noexcept;

    /// Allocate or deallocate disk space for @p fd in the range [@p offset, @p offset + @p len).
    ///
    /// @param mode  fallocate(2) mode flags (0 for a simple pre-allocation,
    ///              FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE to punch a hole, etc.).
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void> fallocate(Fd& fd, int mode, off_t offset, off_t len) noexcept;

    /// Truncate or extend @p fd to exactly @p len bytes.
    ///
    /// Requires kernel 6.9+ and liburing 2.6+.
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void> ftruncate(Fd& fd, off_t len) noexcept;

    /// Suspend the current fiber for the given duration.
    ///
    /// @param duration The duration to sleep for.
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void> sleep_for_ns(std::chrono::nanoseconds duration) noexcept;

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