#pragma once

#include <fcntl.h>
#include <liburing.h>

#include <atomic>
#include <cassert>
#include <concepts>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

#include "buffer_pool.hpp"
#include "detail/fiber.hpp"
#include "detail/mpsc_queue.hpp"
#include "detail/timer_wheel.hpp"
#include "error.hpp"
#include "fd.hpp"
#include "logger.hpp"

namespace tinyio {

/// Default io uring flag
inline constexpr unsigned k_uring_default_additional_flag =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN |
    IORING_SETUP_TASKRUN_FLAG;

/// These are mandatory base flags
inline constexpr unsigned k_uring_base_flags =
    IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP | IORING_SETUP_R_DISABLED;

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

  /// Enables IORING_SETUP_CLAMP when available in the kernel headers.
  bool clamp_ring_sizes = true;

  /// max resume per tick
  std::size_t batch_max_size = 128;

  /// Capacity for the cross-thread post queue (must be power of 2)
  std::size_t post_queue_capacity = 4096;

  /// Direct FD table size (0 to disable)
  std::uint32_t direct_fd_table_size = 1024;

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

class IO final {
 public:
  /// @name Internal scheduler hooks for synchronization primitives.
  /// @note Not for application code. Owner-thread only.
  ///@{
  [[nodiscard]] detail::fiber::Fiber* RunningFiber() const noexcept {
    return current_fiber_;
  }

  /// Park the running fiber. Caller must have enqueued it on a wait list first.
  /// @note Not for application code. Owner-thread only.
  void ParkRunning() noexcept {
    assert(current_fiber_ != nullptr && IsOwnerThread());
    current_fiber_->state = detail::fiber::FiberState::waiting;
    current_fiber_->wait_kind = detail::fiber::WaitKind::sync;
    SuspendFiber(current_fiber_);
  }

  /// Make a previously parked fiber runnable again.
  /// @note Not for application code. Owner-thread only.
  void Unpark(detail::fiber::Fiber* f) noexcept { PushReady(f); }
  ///@}

  /// Constructs an uninitialized Runtime shell; create() performs actual initialization.
  explicit IO(size_t id, const IoOptions& opts, std::vector<BucketConfig> pool_configs);

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
  /// The callable is invoked as fn(IO&). The fiber never migrates to another Runtime.
  /// This method is owner-thread-only. It catches construction/allocation failures and
  /// returns Result.
  template <class Fn>
    requires std::constructible_from<std::move_only_function<void(IO&) noexcept>, Fn>
  [[nodiscard]] Result<FiberId> Spawn(Fn&& fn) noexcept {
    try {
      return SpawnTask(std::move_only_function<void(IO&) noexcept>(std::forward<Fn>(fn)));
    } catch (const std::bad_alloc&) {
      return Fail(ENOMEM, "spawn");
    } catch (...) {
      return Fail(EINVAL, "spawn");
    }
  }

  /// Runs the cooperative scheduler until all fibers finish or stop is requested.
  ///
  /// This method is owner-thread-only. It blocks the owner thread only when no fiber is
  /// ready and the runtime is waiting for io_uring completions.
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
  /// @return true if the task was successfully enqueued; false on OOM.
  template <class Fn>
    requires std::constructible_from<std::move_only_function<void(IO&) noexcept>, Fn>
  [[nodiscard]] bool Post(Fn&& fn) noexcept {
    try {
      if (!post_queue_.Enqueue(
              std::move_only_function<void(IO&) noexcept>(std::forward<Fn>(fn)))) {
        return false;
      }
    } catch (...) {
      return false;
    }
    Wake();
    return true;
  }

  /// Suspend the current fiber to give bck the control to the runtime
  [[nodiscard]] Result<void> Yield() noexcept;

  /// Returns true when there are live fibers still owned by this runtime.
  [[nodiscard]] bool HasLiveFibers() const noexcept;

  /// Checks if we are running an operation on the thread which created this IO instance
  [[nodiscard]] bool IsOwnerThread() const noexcept {
    return std::this_thread::get_id() == owner_tid_;
  }

  /// Returns true if the runtime is currently stopping
  [[nodiscard]] bool Stopping() const noexcept { return stopping_; }

  /// Register a fixed buffer pool
  Result<void> RegisterBuffers(FixedBufferPool& pool) noexcept;
  Result<void> UnregisterBuffers() noexcept;
  [[nodiscard]] Result<FixedBuffer> TakeFixedBuffer(const size_t size) noexcept {
    return buffer_pool_.Take(size);
  }

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
  /// auto buf = TRY(fio.take_fixed_buffer(4096));
  /// std::memcpy(buf.ptr(), data.data(), data.size());
  /// auto n = TRY(fio.write_fixed(fd, buf, data.size(), file_offset));
  /// @endcode
  [[nodiscard]] Result<uint32_t> WriteFixed(const Fd& fd, const FixedBuffer& buf,
                                            size_t len, off_t offset = -1) noexcept;

  /// Write the whole contents of @p buf to @p fd starting at @p offset.
  /// Equivalent to write_fixed(fd, buf, buf.size(), offset).
  [[nodiscard]] Result<uint32_t> WriteFixed(const Fd& fd, const FixedBuffer& buf,
                                            off_t offset = -1) noexcept;
  /// Read up to @p len bytes from @p fd into @p buf (a registered fixed buffer)
  /// starting at @p offset.
  ///
  /// Uses io_uring's READ_FIXED operation.
  /// Pass @p offset = -1 to use the file's current position.
  ///
  /// @return Number of bytes read, or an error code.
  ///
  /// @code
  /// auto buf = TRY(fio.take_fixed_buffer(4096));
  /// auto n = TRY(fio.read_fixed(fd, buf, 4096, 0));
  /// std::string_view content{reinterpret_cast<const char*>(buf.ptr()),
  ///                          static_cast<size_t>(n)};
  /// @endcode
  [[nodiscard]] Result<uint32_t> ReadFixed(const Fd& fd, FixedBuffer& buf, size_t len,
                                           off_t offset = -1) noexcept;

  /// Read up to buf.size() bytes from @p fd into @p buf starting at @p offset.
  /// Equivalent to read_fixed(fd, buf, buf.size(), offset).
  [[nodiscard]] Result<uint32_t> ReadFixed(const Fd& fd, FixedBuffer& buf,
                                           off_t offset = -1) noexcept;

  /// Read up to buf.size() bytes from @p fd into the caller-provided span @p buf.
  ///
  /// Unlike read_fixed, this works with any user buffer but requires the kernel
  /// to copy data through an intermediate bounce buffer.
  /// Pass @p offset = -1 to use the file's current position.
  ///
  /// @return Number of bytes read, or an error code.
  [[nodiscard]] Result<uint32_t> Read(const Fd& fd, std::span<std::byte> buf,
                                      off_t offset = -1) noexcept;

  /// Write up to buf.size() bytes from the caller-provided span @p buf to @p fd.
  ///
  /// Pass @p offset = -1 to use the file's current position.
  ///
  /// @return Number of bytes written, or an error code.
  [[nodiscard]] Result<uint32_t> Write(const Fd& fd, std::span<const std::byte> buf,
                                       off_t offset = -1) noexcept;

  /// Write the scatter-gather vector @p iovecs to @p fd starting at @p offset.
  ///
  /// Wraps pwritev2 via io_uring's WRITEV operation.
  /// Pass @p offset = -1 to use the file's current position.
  ///
  /// @return Total bytes written, or an error code.
  [[nodiscard]] Result<uint32_t> Writev(const Fd& fd, std::span<const iovec> iovecs,
                                        off_t offset = -1) noexcept;

  /// Read the scatter-gather vector @p iovecs from @p fd starting at @p offset.
  ///
  /// Wraps preadv2 via io_uring's READV operation.
  /// Pass @p offset = -1 to use the file's current position.
  ///
  /// @return Total bytes read, or an error code.
  [[nodiscard]] Result<uint32_t> Readv(const Fd& fd, std::span<const iovec> iovecs,
                                       off_t offset = -1) noexcept;

  /// Flush pending writes on @p fd to the storage device.
  ///
  /// @param fd
  /// @param datasync  If true, flushes data only (fdatasync semantics —
  ///                  metadata such as mtime may lag).  If false, flushes
  ///                  both data and metadata (fsync semantics).
  /// @return {} on success, or an error code.
  [[nodiscard]] Result<void> Fsync(const Fd& fd, bool datasync = false) noexcept;

  /// Open or create the file at @p path and return a file descriptor.
  ///
  /// @param path
  /// @param flags  Standard open(2) flags such as O_RDONLY, O_CREAT | O_WRONLY.
  /// @param mode   Permission bits used when O_CREAT creates a new file.
  ///               Ignored if O_CREAT is not set.
  /// @return An owned Fd on success, or an error code (e.g. ENOENT, EACCES).
  ///
  /// @code
  /// auto fd = TRY(fio.open("/var/log/app.log", O_CREAT | O_WRONLY | O_APPEND, 0644));
  /// @endcode
  [[nodiscard]] Result<Fd> Open(const std::filesystem::path& path, int flags,
                                mode_t mode = 0644) noexcept;

  /// Accept a new connection on @p server_fd, retrieving the client address.
  ///
  /// @return An owned Fd on success, or an error code.
  [[nodiscard]] Result<Fd> Accept(const Fd& server_fd, SocketAddress& client_addr,
                                  int flags = k_default_accept_flags) noexcept;

  /// Accept a new connection on @p server_fd without retrieving the client address.
  ///
  /// @return An owned Fd on success, or an error code.
  [[nodiscard]] Result<Fd> Accept(const Fd& server_fd,
                                  int flags = k_default_accept_flags) noexcept;

  /// Accept connections in a continuous background loop using IORING_ACCEPT_MULTISHOT.
  /// Every accepted connection will automatically spawn a new fiber executing @p handler.
  template <class Fn>
    requires std::constructible_from<
        std::move_only_function<void(IO&, Result<Fd>) noexcept>, Fn>
  [[nodiscard]] Result<void> AcceptLoop(Fd server_fd, Fn&& handler) noexcept;

  /// Close @p fd asynchronously via io_uring.
  ///
  /// Takes ownership of @p fd so that it is released exactly once even if the
  /// close CQE returns an error.  Callers should use std::move:
  ///
  /// @code
  /// @brief Manually close a raw file descriptor via io_uring.
  [[nodiscard]] Result<void> Close(int fd) noexcept;

  /// @brief Close an Fd via io_uring. Unregisters if it's a direct Fd.
  [[nodiscard]] Result<void> Close(Fd fd) noexcept;

  /// @brief Registers an Fd as a direct file descriptor to bypass kernel FD lookup
  /// overhead. The input fd is consumed. The resulting Fd will have `is_direct()` true.
  [[nodiscard]] Result<Fd> RegisterFile(Fd fd) noexcept;

  /// @brief Internal: Unregister a direct FD immediately (synchronous syscall).
  void UnregisterDirectFd(int direct_idx) noexcept;

  /// Remove (unlink) the file at @p path.
  [[nodiscard]] Result<void> Remove(const std::filesystem::path& path) noexcept;

  /// Rename the file from @p from to @p to.
  [[nodiscard]] Result<void> Rename(const std::filesystem::path& from,
                                    const std::filesystem::path& to) noexcept;

  /// Poll for the specified events on @p fd.
  ///
  /// @param poll_mask Standard poll(2) mask (e.g. POLLIN, POLLOUT).
  /// @return The triggered events, or an error code.
  [[nodiscard]] Result<uint32_t> Poll(const Fd& fd, unsigned poll_mask) noexcept;

  /// Connect @p fd to the remote address @p addr.
  /// Note: @p addr must outlive this function call.
  [[nodiscard]] Result<void> Connect(const Fd& fd, const SocketAddress& addr) noexcept;

  /// Allocate or deallocate disk space for @p fd in the range [@p offset, @p offset + @p
  /// len).
  ///
  /// @param fd
  /// @param mode  fallocate(2) mode flags (0 for a simple pre-allocation,
  ///              FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE to punch a hole, etc.).
  /// @return {} on success, or an error code.
  [[nodiscard]] Result<void> Fallocate(const Fd& fd, int mode, off_t offset,
                                       off_t len) noexcept;

  /// Truncate or extend @p fd to exactly @p len bytes.
  ///
  /// Requires kernel 6.9+ and liburing 2.6+.
  /// @return {} on success, or an error code.
  [[nodiscard]] Result<void> Ftruncate(const Fd& fd, off_t len) noexcept;

  /// Suspend the current fiber for the given duration.
  ///
  /// @param duration The duration to sleep for.
  /// @return {} on success, or an error code.
  [[nodiscard]] Result<void> SleepForNs(std::chrono::nanoseconds duration) noexcept;

  // =====
  // Timed I/O (timer-wheel based)
  // =====
  // TODO create overload instead of different methods
  // Each *_timed op behaves like its untimed counterpart but fails with
  // ETIMEDOUT if no completion arrives within @p timeout. Timeouts are
  // honored with up to TimerWheel::kTickNs (64ms) of slack. The hot path
  // costs two intrusive-list operations; the kernel is only involved
  // (one ASYNC_CANCEL) when a timeout actually fires.
  [[nodiscard]] Result<uint32_t> Read(const Fd& fd, std::span<std::byte> buf,
                                      std::chrono::nanoseconds timeout,
                                      off_t offset = -1) noexcept;
  [[nodiscard]] Result<uint32_t> Write(const Fd& fd, std::span<const std::byte> buf,
                                       std::chrono::nanoseconds timeout,
                                       off_t offset = -1) noexcept;
  [[nodiscard]] Result<Fd> Accept(const Fd& server_fd, std::chrono::nanoseconds timeout,
                                  int flags = k_default_accept_flags) noexcept;
  [[nodiscard]] Result<void> Connect(const Fd& fd, const SocketAddress& addr,
                                     std::chrono::nanoseconds timeout) noexcept;
  [[nodiscard]] Result<uint32_t> Poll(const Fd& fd, unsigned poll_mask,
                                      std::chrono::nanoseconds timeout) noexcept;

 private:
  static constexpr uint64_t k_wakeup_sentinel = 0xDEAD'C0DE'DEAD'C0DEULL;
  static constexpr uint64_t k_fiber_cancel_sentinel = 0xDEAD'C0DE'DEAD'C0CCULL;
  //
  // IO Uring management
  //
  io_uring ring_{};
  Result<void> SubmitOrWaitFor() noexcept;
  Result<io_uring_sqe*> GetSqe() noexcept;

  Result<void> Activate() noexcept;
  /// Run a single iteration of the event loop (process kernel completions and run ready
  /// fibers).
  Result<void> Tick() noexcept;
  Result<void> ArmWakeRead() noexcept;
  void Wake() const noexcept;

  struct MultishotAcceptState {
    Fd server_fd;
    std::move_only_function<void(IO&, Result<Fd>) noexcept> handler;
  };
  std::vector<uint64_t> active_multishots_;

  Result<std::size_t> DrainCompletions() noexcept;
  void DrainRemote();
  void ResumeReadyFibers();

  // Internal executor for I/O operations. Factorizes the SQE acquisition,
  // fiber suspension, and error mapping logic.
  template <typename T = uint32_t, typename Prep>
  TIO_NO_SANITIZE_ADDRESS Result<T> Syscall(const char* op_name, Prep&& prep) noexcept {
    if (stopping_) {
      return Fail(ECANCELED, op_name);
    }

    // - Prevents multi-threaded corruption of io_uring
    // - Prevents calling io.read() from outside a fiber
    if (!IsOwnerThread() || current_fiber_ == nullptr) {
      return Fail(EPERM, op_name);
    }

    // Internal Sanity Checks
    assert(current_fiber_->runtime == this);
    assert(current_fiber_->state == detail::fiber::FiberState::running);

    auto sqe = TRY_LOG_ERR(GetSqe(), "failed to get sqe: io ops [{}]", op_name);

    detail::fiber::Operation& op = current_fiber_->operation;
    op = detail::fiber::Operation{};
    op.fiber = current_fiber_;
    op.op_name = op_name;

    std::forward<Prep>(prep)(sqe);
    io_uring_sqe_set_data64(sqe, reinterpret_cast<__u64>(&op));

    current_fiber_->state = detail::fiber::FiberState::waiting;
    current_fiber_->wait_kind = detail::fiber::WaitKind::io;
    ++inflight_ops_;

    // suspend now
    SuspendFiber(current_fiber_);

    if (op.cqe_res < 0) {
      return Fail(-op.cqe_res, op_name);
    }

    if constexpr (std::is_same_v<T, void>) {
      return {};
    } else {
      return static_cast<T>(op.cqe_res);
    }
  }

  /// Timed variant of syscall(): identical submission path, plus the fiber
  /// is armed in the timer wheel. No extra SQE, no kernel timer. If the
  /// wheel expires the op first, the resulting -ECANCELED is surfaced as
  /// ETIMEDOUT (via Operation::timed_out).
  template <typename T = uint32_t, typename Prep>
  TIO_NO_SANITIZE_ADDRESS Result<T> SyscallTimed(const char* op_name,
                                                 const std::chrono::nanoseconds timeout,
                                                 Prep&& prep) noexcept {
    if (timeout.count() <= 0) {
      return Syscall<T>(op_name, std::forward<Prep>(prep));
    }

    if (stopping_) {
      return Fail(ECANCELED, op_name);
    }

    if (!IsOwnerThread() || current_fiber_ == nullptr) {
      return Fail(EPERM, op_name);
    }

    assert(current_fiber_->runtime == this);
    assert(current_fiber_->state == detail::fiber::FiberState::running);
    assert(!static_cast<IntrusiveHook<TimerTag>*>(current_fiber_)->IsLinked() &&
           "fiber already armed in the timer wheel");

    auto sqe = TRY_LOG_ERR(GetSqe(), "failed to get sqe: io ops [{}]", op_name);

    detail::fiber::Operation& op = current_fiber_->operation;
    op = detail::fiber::Operation{};
    op.fiber = current_fiber_;
    op.op_name = op_name;

    std::forward<Prep>(prep)(sqe);
    io_uring_sqe_set_data64(sqe, reinterpret_cast<__u64>(&op));

    timer_wheel_.Arm(current_fiber_,
                     detail::MonotonicNowNs() + static_cast<uint64_t>(timeout.count()));

    current_fiber_->state = detail::fiber::FiberState::waiting;
    current_fiber_->wait_kind = detail::fiber::WaitKind::io;
    ++inflight_ops_;

    SuspendFiber(current_fiber_);

    // On resume the fiber is always disarmed: drain_completions() disarms
    // on normal completion; process_expired_timers() disarmed on expiry.
    if (op.cqe_res == -ECANCELED && op.timed_out) {
      return Fail(ETIMEDOUT, op_name);
    }
    if (op.cqe_res < 0) {
      return Fail(-op.cqe_res, op_name);
    }

    if constexpr (std::is_same_v<T, void>) {
      return {};
    } else {
      return static_cast<T>(op.cqe_res);
    }
  }

  /// Cancel every op whose wheel deadline has passed (rare path).
  void ProcessExpiredTimers() noexcept;

  /// Per-op timeout bookkeeping. Owner-thread only.
  detail::TimerWheel timer_wheel_;

  //
  // event fd for cross-thread signaling
  //
  int wake_fd_{-1};
  uint64_t wake_value_{0};

  // Direct FD Table Management
  std::vector<int> direct_fds_;
  uint32_t direct_fds_cnt_ = 0;
  uint32_t next_free_direct_idx_ = 0;

  //
  // Lifecycle
  //
  bool is_activated_{false};
  bool is_running_{false};
  bool stopping_{false};
  std::thread::id owner_tid_;

  //
  // Configs
  //
  std::size_t id_;  ///  Id of the worker in a multi-threaded context
  IoOptions opts_;

  FixedBufferPool buffer_pool_;

  //
  // Fiber management
  //
  detail::context::Context scheduler_context_{};
  detail::ToolFiberState scheduler_tools_{};
  IntrusiveList<detail::fiber::Fiber, AllFibersTag, false> all_fibers_;
  IntrusiveList<detail::fiber::Fiber, ReadyQueueTag, false> ready_queue_;
  IntrusiveList<detail::fiber::Fiber, IdleQueueTag, true> idle_fibers_;  ///< For recycle
  detail::fiber::Fiber* current_fiber_ = nullptr;
  FiberId next_fiber_id_ = 1;
  std::size_t live_fibers_ = 0;
  std::size_t inflight_ops_ = 0;

  /// MPSC queue for tasks posted from remote threads via post().
  /// Drained on the owner thread inside tick().
  detail::MpscQueue<std::move_only_function<void(IO&) noexcept>> post_queue_;

  [[noreturn]] TIO_NO_SANITIZE_ADDRESS void FinishCurrentFiber(
      detail::fiber::Fiber* fiber) noexcept;
  friend void ::TinyioFiberEntry(boost::context::detail::transfer_t) noexcept;

  void PushReady(detail::fiber::Fiber* fiber) noexcept;
  void RemoveReady(detail::fiber::Fiber* fiber) noexcept;
  [[nodiscard]] detail::fiber::Fiber* PopReady() noexcept;
  void DestroyFiber(detail::fiber::Fiber* fiber) noexcept;
  void DestroyAllFibers() noexcept;
  TIO_NO_SANITIZE_ADDRESS void SuspendFiber(detail::fiber::Fiber* fiber) noexcept;
  TIO_NO_SANITIZE_ADDRESS void ResumeFiber(detail::fiber::Fiber* fiber) noexcept;

  //
  // Other private helper methods
  //

  /// Initializes the io_uring instance and validates required opcodes.
  [[nodiscard]] Result<void> Initialize() noexcept;

  /// Issue IORING_OP_ASYNC_CANCEL for every waiting fiber, submit, then drive
  /// tick() until all in-flight ops and ready fibers are drained.
  void CancelAndDrain() noexcept;

  /// Spawns a fiber from an already-erased task object.
  [[nodiscard]] Result<FiberId> SpawnTask(
      std::move_only_function<void(IO&) noexcept> task) noexcept;

  void PinToCpu() const;
};

template <class Fn>
  requires std::constructible_from<
      std::move_only_function<void(IO&, Result<Fd>) noexcept>, Fn>
[[nodiscard]] Result<void> IO::AcceptLoop(Fd server_fd, Fn&& handler) noexcept {
  if (stopping_) return Fail(ECANCELED, "accept_loop");
  if (!server_fd.IsValid()) return Fail(EBADF, "accept_loop: invalid fd");

  auto* state = new (std::nothrow)
      MultishotAcceptState{std::move(server_fd), std::forward<Fn>(handler)};
  if (!state) return Fail(ENOMEM, "accept_loop: OOM allocating state");
  static_assert(alignof(MultishotAcceptState) >= 2,
                "tagged pointer requires even alignment");

  auto* sqe = GetSqe().value_or(nullptr);
  if (!sqe) {
    delete state;
    return Fail(EAGAIN, "accept_loop: ring full");
  }

  io_uring_prep_multishot_accept(sqe, state->server_fd.Get(), nullptr, nullptr, 0);
  if (state->server_fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;

  // Tag the lowest bit to indicate a MultishotAcceptState
  uint64_t tagged_ptr = reinterpret_cast<uint64_t>(state) | 1ULL;
  io_uring_sqe_set_data64(sqe, tagged_ptr);
  active_multishots_.push_back(tagged_ptr);

  inflight_ops_++;
  return {};
}

}  // namespace tinyio