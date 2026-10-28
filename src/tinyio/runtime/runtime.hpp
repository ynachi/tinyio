#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <liburing.h>

#include "tinyio/check.hpp"
#include "tinyio/detail/fiber.hpp"
#include "tinyio/detail/ring_buffer.hpp"
#include "tinyio/detail/timer_wheel.hpp"
#include "tinyio/error.hpp"
#include "tinyio/logging.hpp"
#include "tinyio/net/fd.hpp"
#include "tinyio/runtime/buffer_pool.hpp"

namespace tinyio {

namespace detail {
template <typename T>
class MpscQueue;
}  // namespace detail

template <class Fn>
concept FiberTask =
    std::move_constructible<std::decay_t<Fn>> && std::constructible_from<std::decay_t<Fn>, Fn> &&
    std::is_nothrow_invocable_v<std::decay_t<Fn>&, IO&>;

/// Default io uring flag
inline constexpr unsigned k_uring_default_additional_flag =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG;

/// These are mandatory base flags
inline constexpr unsigned k_uring_base_flags =
    IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP | IORING_SETUP_R_DISABLED;

/// The largest length io_uring accepts for a single read or write.
inline constexpr size_t MAX_RW_LEN = 0x7ffff000;

/// Default io accept flags
inline constexpr int k_default_accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;

struct IoOptions {
    // list of cpus, if empty, no pinning
    std::vector<int> worker_cpu_affinity{};

    /// Additional flags passed to io_uring
    unsigned additional_uring_flags = k_uring_default_additional_flag;

    /// Number of submission queue entries requested from io_uring.
    std::uint32_t sq_entries = 4096;

    /// Default usable stack size for newly spawned fibers.
    std::size_t stack_size = 256 * 1024;

    /// Guard bytes placed below each fiber stack with PROT_NONE.
    std::size_t guard_size = 4096;

    /// max resume per tick
    std::size_t batch_max_size = 128;

    /// Capacity for the cross-thread post queue (must be power of 2)
    std::size_t post_queue_capacity = 4096;

    // Liburing auto wakeup the kernel thread so no need to manually do it
    /// IORING_SETUP_DEFER_TASKRUN is not compatible to SQ_POLL
    /// Also, when SQ_POLL is enabled, make sure to pin work threads and kernel threads
    /// and do them on different CPUs, overwhise, the bench reveals that performance
    /// drops on throughput and latency.
    ///
    /// @warning SQPOLL without registered files (IOSQE_FIXED_FILE) has a
    /// descriptor-lifetime hazard: the kernel's sq thread submits SQEs after
    /// user code has moved on, so closing an fd before its SQE is submitted
    /// makes the kernel resolve a recycled fd number. Registered files pin
    /// the struct file and are the sanctioned fix, but measured to bring no
    /// gain to tinyio's workloads, so they were removed. Treat SQPOLL as
    /// experimental until it is reintroduced together with file registration.
    std::uint32_t sq_thread_idle_ms = 2000;
    // -1 means don't pin to a specific CPU
    int sq_thread_cpu = -1;

    /// Total number of fibers which can be recycle
    std::size_t max_idle_fibers = 1024;

    /// The runtime's fixed buffer pool: one runtime, one pool. When set, the
    /// runtime maps the pool and registers it with the ring at construction;
    /// unset, the fixed-buffer API is unavailable.
    ///
    /// Registered pages are pinned: without CAP_IPC_LOCK they count against
    /// RLIMIT_MEMLOCK (`ulimit -l`), and construction throws ENOMEM past it.
    std::optional<BufferPoolConfig> buffer_pool{};
};

class IO final {
    friend class FixedBuffer;
    friend class FixedBufferPool;
    friend struct Baton;
    friend struct Latch;
    friend struct Mutex;
    friend struct Semaphore;
    template <typename T>
    friend struct TaskGroup;
public:
    /// Constructs an uninitialized Runtime shell; create() performs actual initialization.
    explicit IO(uint32_t id, const IoOptions& opts);

    /// Destroys the runtime and releases ring resources and all fiber stacks.
    ///
    /// Production code should call run() until all spawned fibers complete before
    /// destruction.
    ~IO() noexcept;

    /// Runtime is not copyable because it uniquely owns fibers and an io_uring instance.
    IO(const IO&) = delete;

    /// Runtime is not copy-assignable because ownership is unique and stable.
    IO& operator=(const IO&) = delete;

    /// Runtime is not movable because Fiber objects store their owning Runtime address.
    IO(IO&&) = delete;

    /// Runtime is not move-assignable because address stability is part of the design.
    IO& operator=(IO&&) = delete;

    /// Spawns one stackful fiber owned by this runtime.
    ///
    /// The callable is constructed directly at the top of the fiber's stack
    /// (zero extra heap allocations even for large captures) and invoked as
    /// fn(IO&). The fiber never migrates to another Runtime.
    /// This method is owner-thread-only. It catches construction/allocation failures and
    /// returns Result.
    template <FiberTask Fn>
    [[nodiscard]] Result<FiberId> Spawn(Fn&& fn) noexcept {
        using Task = std::decay_t<Fn>;
        TRY(fiber, AllocateFiber());

        constexpr std::size_t k_align = std::max<std::size_t>(alignof(Task), 16U);
        const auto high = reinterpret_cast<std::uintptr_t>(fiber->stack.high);
        const auto low = reinterpret_cast<std::uintptr_t>(fiber->stack.low);
        if (sizeof(Task) > (high - low) / 2U) {
            RecycleUnstartedFiber(fiber);
            return Error::FailErrno(ENOMEM, "spawn");
        }

        const std::uintptr_t task_addr = (high - sizeof(Task)) & ~(k_align - 1U);
        auto* task_mem = reinterpret_cast<void*>(task_addr);

        Task* placed = nullptr;
        try {
            placed = ::new (task_mem) Task(std::forward<Fn>(fn));
        } catch (const std::bad_alloc&) {
            RecycleUnstartedFiber(fiber);
            return Error::FailErrno(ENOMEM, "spawn");
        } catch (...) {
            RecycleUnstartedFiber(fiber);
            return Error::FailErrno(EINVAL, "spawn");
        }

        fiber->task_ptr = placed;
        fiber->destroy_task = [](void* p) noexcept { static_cast<Task*>(p)->~Task(); };
        return EnqueueFiber(fiber, task_mem, &IO::FiberEntry<Task>);
    }

    /// Runs the cooperative scheduler until stop is requested.
    ///
    /// This method is owner-thread-only. It blocks the owner thread only when no fiber is
    /// ready and the runtime is waiting for io_uring completions.
    ///
    /// Run() can also return WITHOUT a stop request: after k_max_tick_errors
    /// consecutive failed ticks it stops the loop, cancels and drains like a
    /// normal stop, and returns. Tasks Post()ed after that are never run, so a
    /// supervisor that needs the worker alive has to check for this return.
    void Run(const std::stop_token& st) noexcept;

    std::size_t Id() const noexcept { return id_; }

    /// Run until all fibers have completed and the cross-thread post queue is empty.
    ///
    /// This includes tasks enqueued via post() before or during the call.
    void RunUntilDone() noexcept;

    /// Post a callable to this IO's event loop from any thread.
    ///
    /// The callable is enqueued in a lock-free MPSC queue and will be spawned
    /// as a fiber on the next tick() on the owner thread. The owner thread is
    /// woken up via the internal eventfd if it is currently sleeping.
    ///
    /// @return true if the task was successfully enqueued; false on OOM or when stopping.
    template <class Fn>
        requires std::constructible_from<std::move_only_function<void(IO&) noexcept>, Fn>
    [[nodiscard]] bool Post(Fn&& fn) noexcept {
        if ((post_state_.load(std::memory_order_relaxed) & k_post_closed_bit) != 0) {
            return false;
        }
        try {
            return EnqueuePost(std::move_only_function<void(IO&) noexcept>(std::forward<Fn>(fn)));
        } catch (...) {
            return false;
        }
    }

    /// Suspend the current fiber to give back the control to the runtime
    [[nodiscard]] Result<void> Yield() noexcept;

    /// Returns true when there are live fibers still owned by this runtime.
    [[nodiscard]] bool HasLiveFibers() const noexcept;

    /// Checks if we are running an operation on the thread which created this IO instance
    [[nodiscard]] bool IsOwnerThread() const noexcept {
        return std::this_thread::get_id() == owner_tid_;
    }

    /// Returns true if the runtime is currently stopping
    [[nodiscard]] bool Stopping() const noexcept {
        return stopping_.load(std::memory_order_acquire);
    }

    /// @name Fixed buffers
    ///
    /// A fixed buffer is a slot of the runtime's pool (IoOptions::buffer_pool).
    /// The pool is registered with the ring at construction, so a read or a
    /// write can name the slot instead of an address and the kernel does not
    /// have to copy through a bounce buffer.
    ///@{

    /// The runtime's pool, or nullptr when IoOptions::buffer_pool is unset.
    [[nodiscard]] const FixedBufferPool* BufferPool() const noexcept {
        return buffer_pool_ ? &*buffer_pool_ : nullptr;
    }

    /// Take a slot without blocking.
    ///
    /// @return The slot, or EAGAIN when the pool is exhausted.
    /// @note Aborts when the runtime has no pool.
    [[nodiscard]] Result<FixedBuffer> TakeFixedBuffer() noexcept;

    /// Take a slot, parking the running fiber while the pool is empty.
    ///
    /// The fiber wakes when a released slot is handed to it. Like any sync
    /// park, shutdown does not cancel it: a fiber still parked here once the
    /// runtime has quiesced waits for a lease nobody will release, and the
    /// runtime aborts (see AbortIfParkedFibers()).
    /// @note Owner-thread only, inside a fiber. Aborts when the runtime has no
    ///       pool.
    [[nodiscard]] Result<FixedBuffer> AcquireFixedBuffer() noexcept;

    // =====
    // IO Methods
    // =====
    //
    // Read, Write, Accept, Connect and Poll take an optional trailing timeout.
    // With one, the call fails with ETIMEDOUT if no completion arrives in
    // time. Timeouts are honored with up to TimerWheel::k_tick_ns (64ms) of
    // slack. Arming one costs a vector push; the kernel is only involved (one
    // ASYNC_CANCEL) when a timeout actually fires.
    /// Write up to @p len bytes from @p buf (a registered fixed buffer) to @p fd
    /// starting at @p offset.
    ///
    /// Uses io_uring's WRITE_FIXED operation, which avoids the kernel copy into
    /// a pinned buffer because @p buf is already registered with the ring.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// The transfer covers [buffer_offset, buffer_offset + len) of the slot.
    /// A @p buffer_offset outside the slot or a zero @p len is an EINVAL; a
    /// @p len past the end of the slot is clamped to what is left of it.
    ///
    /// @return Number of bytes written, or an error code.
    ///
    /// @code
    /// auto buf = TRY(fio.TakeFixedBuffer());
    /// std::memcpy(buf.Ptr(), data.data(), data.size());
    /// auto n = TRY(fio.WriteFixed(fd, buf, data.size(), file_offset));
    /// @endcode
    [[nodiscard]] Result<uint32_t> WriteFixed(const UniqueFd& fd, const FixedBuffer& buf,
                                              size_t len, off_t file_offset = -1,
                                              size_t buffer_offset = 0,
                                              std::chrono::nanoseconds timeout = {}) noexcept;

    /// Write the whole contents of @p buf to @p fd at its current position.
    /// Equivalent to WriteFixed(fd, buf, buf.Size(), -1, 0).
    [[nodiscard]] Result<uint32_t> WriteFixed(const UniqueFd& fd, const FixedBuffer& buf) noexcept;
    /// Read up to @p len bytes from @p fd into @p buf (a registered fixed buffer)
    /// starting at @p offset.
    ///
    /// Uses io_uring's READ_FIXED operation.
    /// Pass @p file_offset = -1 to use the file's current position.
    ///
    /// Same range contract as WriteFixed().
    ///
    /// @return Number of bytes read, or an error code.
    ///
    /// @code
    /// auto buf = TRY(fio.TakeFixedBuffer());
    /// auto n = TRY(fio.ReadFixed(fd, buf, buf.Size(), 0));
    /// std::string_view content{reinterpret_cast<const char*>(buf.Ptr()),
    ///                          static_cast<size_t>(n)};
    /// @endcode
    [[nodiscard]] Result<uint32_t> ReadFixed(const UniqueFd& fd, FixedBuffer& buf, size_t len,
                                             off_t file_offset = -1, size_t buffer_offset = 0,
                                             std::chrono::nanoseconds timeout = {}) noexcept;

    /// Read up to buf.Size() bytes from @p fd into @p buf at its current position.
    /// Equivalent to ReadFixed(fd, buf, buf.Size(), -1, 0).
    [[nodiscard]] Result<uint32_t> ReadFixed(const UniqueFd& fd, FixedBuffer& buf) noexcept;

    /// Read up to buf.size() bytes from @p fd into the caller-provided span @p buf.
    ///
    /// Unlike read_fixed, this works with any user buffer but requires the kernel
    /// to copy data through an intermediate bounce buffer.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Number of bytes read, or an error code.
    [[nodiscard]] Result<uint32_t> Read(const UniqueFd& fd, std::span<std::byte> buf,
                                        off_t offset = -1,
                                        std::chrono::nanoseconds timeout = {}) noexcept;

    /// Read exactly @p buf.size() bytes from @p fd into @p buf, retrying on short reads.
    /// Fails with EEOF / EIO if EOF is reached before @p buf is filled.
    [[nodiscard]] Result<void> ReadExact(const UniqueFd& fd, std::span<std::byte> buf,
                                         off_t offset = -1,
                                         std::chrono::nanoseconds timeout = {}) noexcept;

    /// Write up to buf.size() bytes from the caller-provided span @p buf to @p fd.
    ///
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Number of bytes written, or an error code.
    [[nodiscard]] Result<uint32_t> Write(const UniqueFd& fd, std::span<const std::byte> buf,
                                         off_t offset = -1,
                                         std::chrono::nanoseconds timeout = {}) noexcept;

    /// Write all @p buf.size() bytes from @p buf to @p fd, retrying on short writes.
    [[nodiscard]] Result<void> WriteAll(const UniqueFd& fd, std::span<const std::byte> buf,
                                        off_t offset = -1,
                                        std::chrono::nanoseconds timeout = {}) noexcept;

    /// Write the scatter-gather vector @p iovecs to @p fd starting at @p offset.
    ///
    /// Wraps pwritev2 via io_uring's WRITEV operation.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Total bytes written, or an error code.
    [[nodiscard]] Result<uint32_t> Writev(const UniqueFd& fd, std::span<const iovec> iovecs,
                                          off_t offset = -1,
                                          std::chrono::nanoseconds timeout = {}) noexcept;

    /// Read the scatter-gather vector @p iovecs from @p fd starting at @p offset.
    ///
    /// Wraps preadv2 via io_uring's READV operation.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Total bytes read, or an error code.
    [[nodiscard]] Result<uint32_t> Readv(const UniqueFd& fd, std::span<const iovec> iovecs,
                                         off_t offset = -1,
                                         std::chrono::nanoseconds timeout = {}) noexcept;

    /// Flush pending writes on @p fd to the storage device.
    ///
    /// @param fd
    /// @param datasync  If true, flushes data only (fdatasync semantics —
    ///                  metadata such as mtime may lag).  If false, flushes
    ///                  both data and metadata (fsync semantics).
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void> Fsync(const UniqueFd& fd, bool datasync = false) noexcept;

    Result<struct statx> Statx(const UniqueFd& fd, unsigned int mask) noexcept;
    Result<struct statx> Statx(const std::filesystem::path& path, unsigned int mask, int flags) noexcept;

    /// Open or create the file at @p path and return a file descriptor.
    ///
    /// @param path
    /// @param flags  Standard open(2) flags such as O_RDONLY, O_CREAT | O_WRONLY.
    /// @param mode   Permission bits used when O_CREAT creates a new file.
    ///               Ignored if O_CREAT is not set.
    /// @return An owned Fd on success, or an error code (e.g. ENOENT, EACCES).
    ///
    /// @code
    /// TRY(fd, fio.Open("/var/log/app.log", O_CREAT | O_WRONLY | O_APPEND, 0644));
    /// @endcode
    [[nodiscard]] Result<UniqueFd> Open(const std::filesystem::path& path, int flags,
                                        mode_t mode = 0644) noexcept;

    /// Accept a new connection on @p server_fd, retrieving the client address.
    ///
    /// @return An owned Fd on success, or an error code.
    [[nodiscard]] Result<UniqueFd> Accept(const UniqueFd& server_fd, SocketAddress& client_addr,
                                          int flags = k_default_accept_flags,
                                          std::chrono::nanoseconds timeout = {}) noexcept;

    /// Accept a new connection on @p server_fd without retrieving the client address.
    ///
    /// @return An owned Fd on success, or an error code.
    [[nodiscard]] Result<UniqueFd> Accept(const UniqueFd& server_fd,
                                          int flags = k_default_accept_flags,
                                          std::chrono::nanoseconds timeout = {}) noexcept;

    /// Close @p fd asynchronously via io_uring.
    ///
    /// Takes ownership of @p fd so that it is released exactly once even if the
    /// close CQE returns an error.  Callers should use std::move:
    ///
    /// @code
    /// @brief Manually close a raw file descriptor via io_uring.
    [[nodiscard]] Result<void> Close(int fd) noexcept;

    /// @brief Close an Fd via io_uring.
    [[nodiscard]] Result<void> Close(UniqueFd fd) noexcept;

    /// Remove (unlink) the file at @p path.
    [[nodiscard]] Result<void> Remove(const std::filesystem::path& path) noexcept;

    /// Rename the file from @p from to @p to.
    [[nodiscard]] Result<void> Rename(const std::filesystem::path& from,
                                      const std::filesystem::path& to) noexcept;

    /// Poll for the specified events on @p fd.
    ///
    /// @param poll_mask Standard poll(2) mask (e.g. POLLIN, POLLOUT).
    /// @return The triggered events, or an error code.
    [[nodiscard]] Result<uint32_t> Poll(const UniqueFd& fd, unsigned poll_mask,
                                        std::chrono::nanoseconds timeout = {}) noexcept;

    /// Connect @p fd to the remote address @p addr.
    /// Note: @p addr must outlive this function call.
    [[nodiscard]] Result<void> Connect(const UniqueFd& fd, const SocketAddress& addr,
                                       std::chrono::nanoseconds timeout = {}) noexcept;

    /// Allocate or deallocate disk space for @p fd in the range [@p offset, @p offset + @p
    /// len).
    ///
    /// @param fd
    /// @param mode  fallocate(2) mode flags (0 for a simple pre-allocation,
    ///              FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE to punch a hole, etc.).
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void> Fallocate(const UniqueFd& fd, int mode, off_t offset,
                                         off_t len) noexcept;

    /// Truncate or extend @p fd to exactly @p len bytes.
    ///
    /// Requires kernel 6.9+ and liburing 2.6+.
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void> Ftruncate(const UniqueFd& fd, off_t len) noexcept;

    /// Suspend the current fiber for the given duration.
    ///
    /// @param duration The duration to sleep for.
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void> SleepForNs(std::chrono::nanoseconds duration) noexcept;

private:
    static constexpr uint64_t k_wakeup_sentinel = 0xDEAD'C0DE'DEAD'C0DEULL;
    static constexpr uint64_t k_fiber_cancel_sentinel = 0xDEAD'C0DE'DEAD'C0CCULL;
    static constexpr uint64_t k_post_closed_bit = 1ULL << 63U;

    /// Consecutive tick failures tolerated before the runtime gives up on a
    /// drain. A tick error used to abandon the drain on the first one, which
    /// left operations in flight while the ring was torn down.
    static constexpr int k_max_tick_errors = 3;

    /// @name Internal scheduler hooks for synchronization primitives.
    /// @note Not for application code. Owner-thread only.
    ///@{
    [[nodiscard]] detail::fiber::Fiber* RunningFiber() const noexcept { return current_fiber_; }

    /// Park the running fiber. Caller must have enqueued it on a wait list first.
    ///
    /// @p reason says WHAT the fiber is parked on. It costs nothing: the
    /// reason is written into the fiber that is parking, and a runtime that
    /// quiesces with parked fibers reports it instead of guessing.
    /// @note Not for application code. Owner-thread only.
    void ParkRunning(const detail::fiber::WaitKind reason) noexcept {
        assert(current_fiber_ != nullptr && IsOwnerThread());
        current_fiber_->state = detail::fiber::FiberState::waiting;
        current_fiber_->wait_kind = reason;
        SuspendFiber(current_fiber_);
    }

    /// Make a sync-parked fiber runnable again.
    /// Aborts if called on a fiber parked in IO::Syscall (WaitKind::io),
    /// preventing premature wakeup while the kernel still holds &fiber->operation.
    void Unpark(detail::fiber::Fiber* f) noexcept {
        CHECK(f != nullptr && f->wait_kind != detail::fiber::WaitKind::io)
            << "IO::Unpark called on a fiber waiting on in-flight kernel I/O";
        f->wait_kind = detail::fiber::WaitKind::none;
        PushReady(f);
    }


    /// Parks the running fiber on @p waiters until someone pops and unparks it.
    ///
    /// A park is not cancelled by shutdown: only the primitive wakes it. A fiber
    /// still parked once the runtime has quiesced has no one left to wake it,
    /// and the runtime aborts (see IO::AbortIfParkedFibers()).
    ///
    /// @p reason names the primitive the fiber is parked on, so a runtime that
    /// aborts can say WHICH primitive is stuck instead of printing a count.
    void ParkOn(FiberWaitQueue& waiters, const detail::fiber::WaitKind reason) noexcept {
        waiters.PushBack(RunningFiber());
        ParkRunning(reason);
    }

    //
    // IO Uring management
    //
    io_uring ring_{};
    /// Submits pending SQEs and, when nothing is runnable, sleeps until a
    /// completion arrives.
    Result<void> SubmitOrWait() noexcept;
    /// Submits the queued SQEs, surviving the transient failures the ring can
    /// produce. @p get_events selects the DEFER_TASKRUN-safe entry point.
    /// @return The number of SQEs submitted, or a negative errno.
    int SubmitSqes(bool get_events) noexcept;
    /// Flushes the kernel's CQ overflow and reaps it, after a submit failed
    /// with EBUSY/EAGAIN.
    void ReapOverflow() noexcept;
    Result<io_uring_sqe*> GetSqe() noexcept;

    Result<void> Activate() noexcept;
    /// Run a single iteration of the event loop (process kernel completions and run ready
    /// fibers).
    Result<void> Tick() noexcept;
    Result<void> ArmWakeRead() noexcept;
    void Wake() noexcept;

    /// Reaps every CQE in the ring; returns how many.
    std::size_t DrainCompletions() noexcept;
    void DrainRemote();
    void ResumeReadyFibers();

    // Aborts unless an I/O call may submit: on the owner thread, inside a
    // fiber. Both are programming errors, not runtime conditions, so they fail
    // loudly instead of returning an error a caller could ignore.
    //
    // - Another thread would corrupt the SQ ring: io_uring_get_sqe() is plain
    //   userspace memory, and SINGLE_ISSUER only rejects the submit, too late.
    //   The thread is checked first, so a foreign thread never reads
    //   current_fiber_.
    // - Outside a fiber there is nothing to suspend.
    void CheckCanSubmit(const char* op_name) const noexcept {
        CHECK(IsOwnerThread()) << op_name << ": called from a thread that does not own this IO";
        CHECK(current_fiber_ != nullptr) << op_name << ": called outside a fiber";
    }

    // Internal executor for I/O operations: SQE acquisition, fiber suspension
    // and error mapping. With a @p timeout, the fiber is also armed in the
    // timer wheel: no extra SQE, no kernel timer. If the wheel expires the op
    // first, the resulting -ECANCELED is surfaced as ETIMEDOUT.
    template <typename T = uint32_t, typename Prep>
    TIO_NO_SANITIZE_ADDRESS Result<T> Syscall(
        const char* op_name, Prep&& prep, const std::chrono::nanoseconds timeout = {}) noexcept {
        CheckCanSubmit(op_name);

        if (stopping_) {
            return Error::FailErrno(ECANCELED, op_name);
        }

        assert(current_fiber_->state == detail::fiber::FiberState::running);

        TRY_LOG_ERR(sqe, GetSqe(), "failed to get sqe: io ops [{}]", op_name);

        detail::fiber::Operation& op = current_fiber_->operation;
        op = detail::fiber::Operation{};
        op.fiber = current_fiber_;

        std::forward<Prep>(prep)(sqe);
        io_uring_sqe_set_data64(sqe, reinterpret_cast<__u64>(&op));

        if (timeout.count() > 0) {
            timer_wheel_.Arm(current_fiber_, NowNs() + static_cast<uint64_t>(timeout.count()));
        }

        current_fiber_->state = detail::fiber::FiberState::waiting;
        current_fiber_->wait_kind = detail::fiber::WaitKind::io;
        ++inflight_ops_;

        SuspendFiber(current_fiber_);

        // On resume the fiber is always disarmed: DrainCompletions() disarms on
        // completion, ProcessExpiredTimers() on expiry.
        if (op.cqe_res < 0) {
            const int err = (op.cqe_res == -ECANCELED && op.timed_out) ? ETIMEDOUT : -op.cqe_res;
            return Error::FailErrno(err, op_name);
        }

        if constexpr (std::is_same_v<T, void>) {
            return {};
        } else {
            return static_cast<T>(op.cqe_res);
        }
    }

    /// Cancel every op whose wheel deadline has passed (rare path).
    void ProcessExpiredTimers() noexcept;

    /// Lazily populated monotonic timestamp for the current tick's Phase 1,
    /// avoiding repeated clock_gettime calls when many fibers arm timeouts.
    [[nodiscard]] uint64_t NowNs() noexcept {
        if (cached_now_ns_ == 0) {
            cached_now_ns_ = detail::MonotonicNowNs();
        }
        return cached_now_ns_;
    }

    [[nodiscard]] bool HasPendingPosts() const noexcept {
        return (post_state_.load(std::memory_order_acquire) & ~k_post_closed_bit) != 0;
    }

    /// Per-op timeout bookkeeping. Owner-thread only.
    detail::TimerWheel timer_wheel_;
    uint64_t cached_now_ns_ = 0;

    //
    // event fd for cross-thread signaling
    //
    // wake_mutex_ serializes Wake() against the destructor: a thread that is
    // still inside Wake() must not write to an eventfd the destructor has
    // already closed (and whose number may already have been recycled).
    //
    int wake_fd_{-1};
    uint64_t wake_value_{0};
    std::mutex wake_mutex_;

    //
    // Lifecycle
    //
    bool is_activated_{false};
    /// When true, a tick returns instead of sleeping once the runtime has
    /// nothing left to do. RunUntilDone() and CancelAndDrain() turn this on:
    /// those callers drive their own loop and decide when to stop, so a tick
    /// must not block on a completion that can never arrive.
    bool drain_to_completion_{false};
    std::atomic<bool> stopping_{false};
    /// Low 63 bits: pending posted tasks; bit 63 (k_post_closed_bit): closed.
    std::atomic<uint64_t> post_state_{0};
    std::thread::id owner_tid_;

    //
    // Configs
    //
    std::uint32_t id_;  ///  Id of the worker in a multi-threaded context
    IoOptions opts_;

    /// The fixed buffer pool, registered with the ring (IoOptions::buffer_pool).
    std::optional<FixedBufferPool> buffer_pool_;

    //
    // Fiber management
    //
    detail::context::Context scheduler_context_{};
    detail::ToolFiberState scheduler_tools_{};
    /// Every fiber this runtime owns, running, waiting or idle. Unordered:
    /// removal swaps the last fiber into the hole (Fiber::all_fibers_index).
    std::vector<detail::fiber::Fiber*> all_fibers_;
    /// Fibers waiting for CPU time, oldest first.
    detail::RingBuffer<detail::fiber::Fiber*> ready_queue_{1024};
    /// Finished fibers kept with their stack for reuse (LIFO).
    std::vector<detail::fiber::Fiber*> idle_fibers_;
    detail::fiber::Fiber* current_fiber_ = nullptr;
    FiberId next_fiber_id_ = 1;
    std::size_t live_fibers_ = 0;
    std::size_t inflight_ops_ = 0;

    /// MPSC queue for tasks posted from remote threads via post().
    /// Drained on the owner thread inside tick().
    std::unique_ptr<detail::MpscQueue<std::move_only_function<void(IO&) noexcept>>> post_queue_;

    [[nodiscard]] bool EnqueuePost(std::move_only_function<void(IO&) noexcept> task) noexcept;

    [[noreturn]] TIO_NO_SANITIZE_ADDRESS void FinishCurrentFiber(
        detail::fiber::Fiber* fiber) noexcept;
    static TIO_NO_SANITIZE_ADDRESS void EnterFiber(detail::fiber::Fiber* fiber) noexcept;

    /// Typed entry point for a fiber whose callable of type @p Task lives on its stack.
    template <class Task>
    [[noreturn]] TIO_NO_SANITIZE_ADDRESS static void FiberEntry(void* arg) noexcept {
        auto* fiber = static_cast<detail::fiber::Fiber*>(arg);
        EnterFiber(fiber);
        auto* task = static_cast<Task*>(fiber->task_ptr);
        (void)(*task)(*fiber->runtime);
        fiber->DestroyTask();
        fiber->runtime->FinishCurrentFiber(fiber);
    }

    [[nodiscard]] Result<detail::fiber::Fiber*> AllocateFiber() noexcept;
    void RecycleUnstartedFiber(detail::fiber::Fiber* fiber) noexcept;
    [[nodiscard]] FiberId EnqueueFiber(detail::fiber::Fiber* fiber, void* stack_top,
                                       detail::context::EntryFn entry) noexcept;

    void PushReady(detail::fiber::Fiber* fiber) noexcept;
    [[nodiscard]] detail::fiber::Fiber* PopReady() noexcept;
    void RemoveFromAllFibers(detail::fiber::Fiber* fiber) noexcept;
    void DestroyFiber(detail::fiber::Fiber* fiber) noexcept;
    void DestroyAllFibers() noexcept;
    TIO_NO_SANITIZE_ADDRESS void SuspendFiber(detail::fiber::Fiber* fiber) noexcept;
    TIO_NO_SANITIZE_ADDRESS void ResumeFiber(detail::fiber::Fiber* fiber) noexcept;

    //
    // Other private helper methods
    //

    /// Initializes the io_uring instance and validates required opcodes.
    [[nodiscard]] Result<void> Initialize() noexcept;

    /// Checks that @p buf is a slot of this runtime's pool (aborts if not) and
    /// returns the transfer length for [buffer_offset, buffer_offset + len),
    /// clamped to the slot; EINVAL for an offset outside it or a zero length.
    [[nodiscard]] Result<unsigned int> FixedRange(const FixedBuffer& buf, size_t len,
                                                  size_t buffer_offset,
                                                  const char* op_name) const noexcept;

    /// Cancels every in-flight request with one SQE, then drives the loop to
    /// quiescence.
    void CancelAndDrain() noexcept;

    /// Runs one tick, counting consecutive failures in @p consecutive_errors.
    /// @return false once k_max_tick_errors ticks in a row have failed.
    bool TickCounted(int& consecutive_errors, const char* who) noexcept;

    /// Ticks until nothing is ready, in flight or posted. Aborts if the ticks
    /// keep failing: returning would tear the ring down under in-flight ops.
    void DriveToQuiescence(const char* who) noexcept;

    /// Aborts if a fiber is still live once the runtime has quiesced: it is
    /// parked on a sync primitive that nothing can ever signal (a deadlock).
    void AbortIfParkedFibers() const noexcept;

    void PinToCpu() const;
};

}  // namespace tinyio