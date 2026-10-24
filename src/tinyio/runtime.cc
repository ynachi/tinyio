#include "runtime.hpp"

#include <sched.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <format>
#include <memory>
#include <utility>

#include <sys/eventfd.h>
#include <ylt/easylog.hpp>

namespace tinyio {
IO::IO(const size_t id, const IoOptions& opts, std::vector<BucketConfig> pool_configs)
    : id_(id),
      opts_(opts),
      buffer_pool_(std::move(pool_configs)),
      post_queue_(opts.post_queue_capacity) {
  if (auto res = Initialize(); !res) {
    const auto err = res.error();
    throw std::system_error(err.code,
                            std::format("tinyio::IO init failed in {}", err.context()));
  }
}

IO::~IO() noexcept {
  const bool was_stopping = stopping_.exchange(true, std::memory_order_release);

  // Discard any tasks that were posted but not yet spawned.
  // Their destructors release any captured resources before we tear down the ring.
  {
    ELOGFMT(INFO, "discarding ready tasks, worker {}", id_);
    std::move_only_function<void(IO&) noexcept> fn;
    while (post_queue_.TryDequeue(fn)) {
    }
  }

  // Quiesce in-flight work before releasing kernel resources.
  //
  // If the ring was activated and there is still live work (fibers waiting
  // on io_uring CQEs, ready fibers not yet resumed), we must cancel and drain
  // here so the kernel does not reference freed Operation* pointers after
  // io_uring_queue_exit().
  //
  // This MUST run on the owner thread: IORING_SETUP_SINGLE_ISSUER forbids
  // cross-thread submission.  Destroying the IO from another thread while
  // the ring has live work is a kernel UAF and causes abort.
  const bool has_live_work =
      live_fibers_ > 0 || inflight_ops_ > 0 || !ready_queue_.Empty();

  if (is_activated_ && !was_stopping && has_live_work) {
    if (!IsOwnerThread()) {
      ELOGFMT(ERROR,
              "IO #{} destroyed from a non-owner thread with {} live fiber(s) "
              "and {} op(s) in-flight — aborting to prevent kernel UAF",
              id_, live_fibers_, inflight_ops_);
      std::abort();
    }

    ELOGFMT(WARN, "IO #{} destroyed with {} live fiber(s); issuing cancels and draining",
            id_, live_fibers_);
    CancelAndDrain();
  }

  if (live_fibers_ > 0) {
    ELOGFMT(WARN, "IO #{} teardown: {} fiber(s) still live (stuck in app logic?)", id_,
            live_fibers_);
  }

  if (ring_.ring_fd > 0 && buffer_pool_.IsRegistered()) {
    if (auto res = UnregisterBuffers(); !res.has_value()) {
      auto err = res.error();
      ELOGFMT(ERROR, "Failed to unregister buffers: [{}] {}", err.context(),
              err.message());
    }
    ELOGFMT(INFO, "unregistered buffers, worker: {}", id_);
  }

  if (ring_.ring_fd > 0 && !direct_fds_.empty()) {
    if (const int ret = io_uring_unregister_files(&ring_); ret < 0) {
      ELOGFMT(ERROR, "Failed to unregister files: {}", std::strerror(-ret));
    }
    ELOGFMT(INFO, "unregistered files, worker: {}", id_);
  }

  if (ring_.ring_fd > 0) {
    io_uring_queue_exit(&ring_);
    ring_.ring_fd = -1;
    ELOGFMT(INFO, "exited io-uring ring, worker {}", id_);
  }

  if (wake_fd_ >= 0) {
    // Take the same lock Wake() uses: a poster may still be inside write().
    std::lock_guard<std::mutex> lock(wake_mutex_);
    if (wake_fd_ >= 0) {
      ::close(wake_fd_);
      wake_fd_ = -1;
    }
  }

  // Release all fiber stacks and Fiber objects (both live and idle).
  DestroyAllFibers();
}

Result<void> IO::Initialize() noexcept {
  io_uring_params params{};

  owner_tid_ = std::this_thread::get_id();

  // disable the ring first (kUringBaseFlags already includes IORING_SETUP_R_DISABLED)
  params.flags |= k_uring_base_flags | opts_.additional_uring_flags;

  params.cq_entries = opts_.sq_entries * 2;

  // Safely configure SQPOLL if requested
  if (params.flags & IORING_SETUP_SQPOLL) {
    params.sq_thread_idle = opts_.sq_thread_idle_ms;
    if (opts_.sq_thread_cpu >= 0) {
      params.flags |= IORING_SETUP_SQ_AFF;
      params.sq_thread_cpu = static_cast<std::uint32_t>(opts_.sq_thread_cpu);
    }
  }

  if ((params.flags & IORING_SETUP_SQPOLL) &&
      (params.flags & IORING_SETUP_DEFER_TASKRUN)) {
    return Error::fail_errno(
        EINVAL,
        "IORING_SETUP_SQPOLL is incompatible with DEFER_TASKRUN; "
        "clear kUringDefaultAdditionalFlag bits when enabling SQPOLL");
  }

  if (const int rc = io_uring_queue_init_params(opts_.sq_entries, &ring_, &params);
      rc < 0) {
    return Error::fail_errno(-rc, "io_uring_queue_init_params");
  }

  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wake_fd_ < 0) {
    io_uring_queue_exit(&ring_);
    return Error::fail_errno(errno, "eventfd");
  }

  // register pool if not empty
  if (buffer_pool_.BucketCount() != 0) {
    TRY_VOID(RegisterBuffers(buffer_pool_));
    ELOGFMT(INFO, "the buffer pool have been registered to io_uring");
  }

  if (opts_.direct_fd_table_size > 0) {
    direct_fds_.resize(opts_.direct_fd_table_size, -1);
    if (const int ret = io_uring_register_files(
            &ring_, direct_fds_.data(), static_cast<unsigned int>(direct_fds_.size()));
        ret < 0) {
      return Error::fail_errno(-ret, "io_uring_register_files");
    }
    // Populate the LIFO free-list in reverse so index 0 is popped first.
    direct_free_.reserve(opts_.direct_fd_table_size);
    for (uint32_t i = opts_.direct_fd_table_size; i-- > 0;) {
      direct_free_.push_back(i);
    }
    ELOGFMT(INFO, "registered direct fd table of size {}", opts_.direct_fd_table_size);
  }

  ELOGFMT(INFO, "Initialized IO context with {} submission entries (SQPOLL: {})",
          opts_.sq_entries,
          (opts_.additional_uring_flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");

  return {};
}

Result<void> IO::RegisterBuffers(FixedBufferPool& pool) noexcept {
  if (pool.IsRegistered()) {
    return Error::fail_errno(EALREADY, "register_buffers: pool already registered");
  }

  if (const int ret = io_uring_register_buffers(
          &ring_, pool.IovecsPtr(), static_cast<unsigned int>(pool.TotalCapacity()));
      ret < 0) {
    return Error::fail_errno(-ret, "io_uring_register_buffers");
  }

  pool.SetRegistered();

  return {};
}

Result<void> IO::UnregisterBuffers() noexcept {
  if (const int ret = io_uring_unregister_buffers(&ring_); ret < 0) {
    return Error::fail_errno(-ret, "io_uring_unregister_buffers");
  }
  buffer_pool_.SetUnregistered();
  return {};
}

Result<UniqueFd> IO::RegisterFile(UniqueFd fd) noexcept {
  assert(IsOwnerThread() && "This method must be called from the IO owner thread");
  if (!fd.Valid() || fd.IsDirect()) return fd;

  if (direct_free_.empty()) {
    if (!direct_fds_.empty()) {
      ELOGFMT(WARN,
              "register_file: direct FD table full ({}/{}), fd {} will use indirect path",
              direct_fds_.size() - direct_free_.size(), direct_fds_.size(), fd.Get());
    }
    return fd;
  }

  const uint32_t idx = direct_free_.back();
  int raw_fd = fd.Get();

  if (const int ret = io_uring_register_files_update(&ring_, idx, &raw_fd, 1); ret < 0) {
    ELOGFMT(WARN, "register_file: io_uring_register_files_update failed for fd {}: {}",
            raw_fd, std::strerror(-ret));
    return fd;
  }

  direct_free_.pop_back();
  direct_fds_[idx] = raw_fd;

  ::close(fd.Release());  // raw kernel fd is consumed

  // A direct fd holds a table index, not a descriptor: give it the no-op
  // closer so the destructor never closes an unrelated descriptor.
  UniqueFd direct_fd(static_cast<int>(idx), UniqueFd::NoopCloser);
  direct_fd.SetDirect(true);
  return direct_fd;
}

void IO::UnregisterDirectFd(const int direct_idx) noexcept {
  assert(IsOwnerThread() && "This method must be called from the IO owner thread");
  if (direct_idx < 0 || static_cast<size_t>(direct_idx) >= direct_fds_.size()) {
    return;
  }

  constexpr int empty_fd = -1;
  if (const int ret = io_uring_register_files_update(
          &ring_, static_cast<unsigned int>(direct_idx), &empty_fd, 1);
      ret >= 0) {
    direct_fds_[static_cast<std::size_t>(direct_idx)] = -1;
    direct_free_.push_back(static_cast<uint32_t>(direct_idx));
  }
}

Result<void> IO::Close(const int fd) noexcept {
  return Syscall<void>("close", [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd); });
}

Result<void> IO::Close(UniqueFd fd) noexcept {
  assert(IsOwnerThread() && "This method must be called from the IO owner thread");
  if (!fd.Valid()) return {};

  if (fd.IsDirect()) {
    UnregisterDirectFd(fd.Get());
    (void)fd.Release();
    return {};
  }

  return Syscall<void>(
      "close", [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd.Release()); });
}

//
// Fiber management
//
void IO::PushReady(detail::fiber::Fiber* fiber) noexcept {
  if (fiber == nullptr || fiber->state == detail::fiber::FiberState::done) {
    return;
  }

  // Already queued: it will run once, however many wakeups it got.
  if (fiber->in_ready_queue) {
    return;
  }

  fiber->state = detail::fiber::FiberState::ready;
  fiber->in_ready_queue = true;
  ready_queue_.Push(fiber);
}

[[nodiscard]] detail::fiber::Fiber* IO::PopReady() noexcept {
  detail::fiber::Fiber* fiber = nullptr;
  if (!ready_queue_.Pop(fiber)) {
    return nullptr;
  }
  fiber->in_ready_queue = false;
  return fiber;
}

void IO::RemoveFromAllFibers(detail::fiber::Fiber* fiber) noexcept {
  // Swap-and-pop: move the last fiber into the hole.
  detail::fiber::Fiber* last = all_fibers_.back();
  all_fibers_[fiber->all_fibers_index] = last;
  last->all_fibers_index = fiber->all_fibers_index;
  all_fibers_.pop_back();
}

void IO::DestroyFiber(detail::fiber::Fiber* fiber) noexcept {
  if (fiber == nullptr) {
    return;
  }

  // The ready queue cannot remove from the middle, so it must not hold a
  // finished fiber. It cannot: a fiber finishes while it runs, after
  // PopReady() took it out, and PushReady() refuses a done fiber.
  assert(!fiber->in_ready_queue && "finished fiber still in the ready queue");
  timer_wheel_.Disarm(fiber);
  fiber->task = nullptr;

  detail::ToolsDestroyFiber(fiber->tools);

  // Enforce the High-Water Mark!
  if (idle_fibers_.size() >= opts_.max_idle_fibers) {
    // only remove from all_fibers_ when we are returning memory to the OS!
    RemoveFromAllFibers(fiber);

    //  unmap the stack memory to prevent leaks
    detail::stack::ReleaseStack(fiber->stack);
    delete fiber;
  } else {
    // Cache it for later. It remains in all_fibers_!
    idle_fibers_.push_back(fiber);
  }

  --live_fibers_;
}

void IO::DestroyAllFibers() noexcept {
  // all_fibers_ owns every fiber, idle ones included, so the other
  // containers only need to forget their pointers.
  for (detail::fiber::Fiber* fiber : all_fibers_) {
    detail::ToolsDestroyFiber(fiber->tools);
    detail::stack::ReleaseStack(fiber->stack);
    delete fiber;
  }
  all_fibers_.clear();
  idle_fibers_.clear();
  ready_queue_.Clear();
}

Result<FiberId> IO::SpawnTask(std::move_only_function<void(IO&) noexcept> task) noexcept {
  if (!IsOwnerThread()) {
    return Error::fail_errno(EPERM, "spawn");
  }

  detail::fiber::Fiber* fiber = nullptr;
  if (!idle_fibers_.empty()) {
    fiber = idle_fibers_.back();
    idle_fibers_.pop_back();
  } else {
    // Use unique_ptr so the Fiber is freed if allocate_stack fails below.
    // We do NOT add to all_fibers_ until the stack is successfully allocated.
    std::unique_ptr<detail::fiber::Fiber> owned(new (std::nothrow)
                                                    detail::fiber::Fiber());
    if (!owned) {
      return Error::fail_errno(ENOMEM, "spawn");
    }
    owned->runtime = this;

    TRY(stack, detail::stack::AllocateStack(opts_.stack_size, opts_.guard_size));
    owned->stack = stack;

    // Only push to all_fibers_ once all allocations have succeeded.
    owned->all_fibers_index = all_fibers_.size();
    all_fibers_.push_back(owned.get());
    fiber = owned.release();
  }

  ++live_fibers_;

  fiber->runtime = this;

  //  Reset state  for all fibers
  // give every execution a unique ID for logging!
  fiber->id = next_fiber_id_++;
  fiber->task = std::move(task);
  fiber->state = detail::fiber::FiberState::ready;
  detail::ToolsCreateFiber(fiber->tools);

  detail::context::Make(fiber->context, fiber->stack.high, &IO::FiberEntry, fiber);

  // push to active list
  PushReady(fiber);

  return fiber->id;
}

TIO_NO_SANITIZE_ADDRESS void IO::ResumeFiber(detail::fiber::Fiber* fiber) noexcept {
  // Mark this fiber as the currently running one
  current_fiber_ = fiber;

  fiber->state = detail::fiber::FiberState::running;

  detail::ToolsBeforeSwitch(scheduler_tools_, &fiber->tools, &fiber->stack);
  detail::context::Switch(scheduler_context_, fiber->context);
  detail::ToolsAfterSwitch(scheduler_tools_);

  // Clear it when we jump back to the scheduler
  current_fiber_ = nullptr;
}

TIO_NO_SANITIZE_ADDRESS void IO::SuspendFiber(detail::fiber::Fiber* fiber) noexcept {
  detail::ToolsBeforeSwitch(fiber->tools, &scheduler_tools_, nullptr);
  detail::context::Switch(fiber->context, scheduler_context_);
  detail::ToolsAfterSwitch(fiber->tools);
}

[[nodiscard]] Result<void> IO::Yield() noexcept {
  // Check 1: Did the user try to cheat and call this from a different OS thread?
  // Check 2: Did the user call this from main() instead of inside a fiber?
  if (!IsOwnerThread() || current_fiber_ == nullptr) {
    return Error::fail_errno(EPERM, "yield");
  }

  assert(current_fiber_->runtime == this);
  assert(current_fiber_->state == detail::fiber::FiberState::running);

  if (stopping_) {
    return Error::fail_errno(ECANCELED, "yield");
  }

  PushReady(current_fiber_);
  SuspendFiber(current_fiber_);

  return {};
}

bool IO::HasLiveFibers() const noexcept { return live_fibers_ > 0; }

[[noreturn]] TIO_NO_SANITIZE_ADDRESS void IO::FinishCurrentFiber(
    detail::fiber::Fiber* fiber) noexcept {
  fiber->state = detail::fiber::FiberState::done;

  detail::ToolsBeforeSwitch(fiber->tools, &scheduler_tools_, nullptr);
  // Last switch out. The stack pointer saved into fiber->context is never
  // resumed: a recycled fiber gets a fresh frame from context::Make().
  detail::context::Switch(fiber->context, scheduler_context_);

  std::terminate();
}

//
// IO Uring methods
//

void IO::CancelAndDrain() noexcept {
  bool issued_cancels = false;

  // Pass 1: Cancel all in-flight IO operations.
  // We do NOT wake up sync-parked fibers here. They might be waiting for
  // the IO fibers to finish (e.g., TaskGroup::Wait, Latch::Wait).
  // Waking them up prematurely would cause them to destroy synchronization
  // primitives while the IO fibers are still unwinding, causing Use-After-Free.
  for (detail::fiber::Fiber* fiber : all_fibers_) {
    if (fiber->state != detail::fiber::FiberState::waiting ||
        fiber->wait_kind != detail::fiber::WaitKind::io) {
      continue;
    }

    auto res = GetSqe();
    if (!res) {
      ELOGFMT(ERROR, "cancel SQE alloc failed: [{}] {}", res.error().context(),
              res.error().message());
      continue;
    }
    io_uring_prep_cancel64(res.value(), reinterpret_cast<__u64>(&fiber->operation), 0);
    io_uring_sqe_set_data64(res.value(), k_fiber_cancel_sentinel);
    ++inflight_ops_;
    issued_cancels = true;
  }

  for (uint64_t tagged_ptr : active_multishots_) {
    auto res = GetSqe();
    if (!res) {
      ELOGFMT(ERROR, "cancel SQE alloc failed: [{}] {}", res.error().context(),
              res.error().message());
      continue;
    }
    io_uring_prep_cancel64(res.value(), tagged_ptr, 0);
    io_uring_sqe_set_data64(res.value(), k_fiber_cancel_sentinel);
    ++inflight_ops_;
    issued_cancels = true;
  }
  active_multishots_.clear();

  if (issued_cancels) {
    io_uring_submit(&ring_);
  }

  // Drain all IO completions and ready fibers. This allows cancelled IO fibers
  // to naturally wake up their sync-parked dependents.
  //
  // drain_to_completion_ keeps a tick from sleeping once the runtime is
  // quiesced: the loop below is the one that decides when to stop, so a tick
  // that blocked here would wait for a completion that can never arrive.
  const bool prev_drain_to_completion = drain_to_completion_;
  drain_to_completion_ = true;
  while (inflight_ops_ > 0 || !ready_queue_.Empty()) {
    if (auto res = Tick(); !res) {
      ELOGFMT(ERROR, "tick failed during cancel_and_drain: [{}] {}",
              res.error().context(), res.error().message());
      break;
    }
  }
  drain_to_completion_ = prev_drain_to_completion;

  // Pass 2: Forcefully wake up any abandoned sync-parked fibers.
  // At this point, there are no active IO operations. Any fiber still
  // waiting is permanently deadlocked (e.g., waiting on a Latch that no
  // one will count down because the counting fiber propagated an ECANCELED).
  // We MUST wake them up so their stacks unwind and RAII destructors run.
  //
  // Each one is still in its primitive's wait queue. It removes itself when
  // it wakes up and sees Stopping() (see ParkOn() in fiber_sync.hpp), so
  // the primitive's destructor finds an empty queue.
  //
  // Iterating all_fibers_ while waking fibers is safe: PushReady() only
  // touches the ready queue, and the fibers run later, in Tick().
  bool abandoned_sync_fibers = false;
  for (detail::fiber::Fiber* fiber : all_fibers_) {
    if (fiber->state == detail::fiber::FiberState::waiting &&
        fiber->wait_kind == detail::fiber::WaitKind::sync) {
      PushReady(fiber);
      abandoned_sync_fibers = true;
    }
  }

  if (abandoned_sync_fibers) {
    ELOGFMT(WARN, "IO #{} teardown: forcefully waking up abandoned sync-parked fibers",
            id_);
    while (inflight_ops_ > 0 || !ready_queue_.Empty()) {
      if (auto res = Tick(); !res) {
        ELOGFMT(ERROR, "tick failed during cancel_and_drain pass 2: [{}] {}",
                res.error().context(), res.error().message());
        break;
      }
    }
  }
}

// First code a new fiber runs, on its own stack, reached through the
// trampoline in context.cc. ResumeFiber() saved the scheduler's context in
// the switch that got us here, so there is nothing to record.
[[noreturn]] void IO::FiberEntry(void* arg) noexcept {
  auto* fiber = static_cast<detail::fiber::Fiber*>(arg);
  if (fiber == nullptr || fiber->runtime == nullptr) {
    std::terminate();
  }

  auto& rt = *fiber->runtime;

  // Complete the ASAN fiber switch ResumeFiber() started. A brand-new fiber
  // has no fake stack to restore: fiber->tools.asan_fake_stack is still the
  // nullptr ToolsCreateFiber() set.
  detail::ToolsAfterSwitch(fiber->tools);
  fiber->task(rt);
  rt.FinishCurrentFiber(fiber);
}

}  // namespace tinyio

namespace tinyio {
Result<void> IO::SubmitOrWaitFor() noexcept {
  // Only block if we have no local work to do.
  const bool runnable =
      io_uring_cq_ready(&ring_) > 0 || !ready_queue_.Empty() || !post_queue_.Empty();
  return SubmitOrWait(runnable);
}

Result<void> IO::SubmitOrWait(const bool runnable) noexcept {
  if (runnable) {
    int ret = 0;
    if (opts_.additional_uring_flags & IORING_SETUP_DEFER_TASKRUN) {
      ret = io_uring_submit_and_get_events(&ring_);
    } else {
      ret = io_uring_submit(&ring_);
    }

    if (ret < 0 && ret != -EINTR) {
      return Error::fail_errno(-ret, "submit_or_wait_for.io_uring_submit");
    }
  } else {
    // Sleep until a CQE arrives, the eventfd is written, or the nearest
    // armed wheel deadline passes. The bound is a syscall argument
    // (IORING_ENTER_EXT_ARG) - no timeout SQE exists anywhere.
    __kernel_timespec ts{};
    __kernel_timespec* tsp = nullptr;
    if (const auto ns = timer_wheel_.NsUntilNext(detail::MonotonicNowNs());
        ns.has_value()) {
      ts.tv_sec = static_cast<long long>(*ns / 1'000'000'000ULL);
      ts.tv_nsec = static_cast<long long>(*ns % 1'000'000'000ULL);
      tsp = &ts;
    }

    io_uring_cqe* cqe = nullptr;
    if (const auto ret = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, tsp, nullptr);
        ret < 0 && ret != -EINTR && ret != -ETIME) {
      return Error::fail_errno(-ret,
                               "submit_or_wait_for.io_uring_submit_and_wait_timeout");
    }
  }

  return {};
}

Result<io_uring_sqe*> IO::GetSqe() noexcept {
  io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (sqe == nullptr) {
    // SQ is full. Try one submit to clear space.
    io_uring_submit(&ring_);
    sqe = io_uring_get_sqe(&ring_);
  }

  // its a fatal error if its null again
  if (sqe == nullptr) {
    return Error::fail_errno(ENOSPC, "get_sqe");
  }
  return sqe;
}

void IO::Wake() noexcept {
  constexpr uint64_t one = 1;
  for (;;) {
    // The lock keeps the write from racing with the destructor's close().
    std::lock_guard<std::mutex> lock(wake_mutex_);
    if (wake_fd_ < 0) {
      // The IO is being torn down; there is nobody left to wake up.
      return;
    }
    const ssize_t n = ::write(wake_fd_, &one, sizeof(one));
    if (n == sizeof(one) || (n == -1 && errno == EAGAIN)) {
      return;
    }
    if (n == -1 && errno == EINTR) {
      continue;
    }
    ELOGFMT(WARN, "failed to wake io context with eventfd: {}", std::strerror(errno));
    return;
  }
}

Result<void> IO::ArmWakeRead() noexcept {
  TRY(sqe, GetSqe());
  io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
  io_uring_sqe_set_data64(sqe, k_wakeup_sentinel);
  io_uring_submit(&ring_);
  return {};
}

Result<void> IO::Activate() noexcept {
  if (is_activated_) {
    ELOGFMT(DEBUG, "IO is already activated, this is a noop");
    return {};
  }

  // Register the scheduler (this OS thread) with the sanitizer runtimes
  // BEFORE the first context switch. Under TSAN, every fiber-to-scheduler
  // switch targets scheduler_tools_.tsan_fiber; if it was never captured,
  // tools_before_switch() silently skips __tsan_switch_to_fiber and TSAN's
  // shadow state diverges from reality on the first suspend - SEGV.
  detail::ToolsCaptureCurrentThread(scheduler_tools_);

  if (const int ret = io_uring_register(
          static_cast<unsigned int>(ring_.ring_fd),
          static_cast<unsigned int>(IORING_REGISTER_ENABLE_RINGS), nullptr, 0);
      ret < 0) {
    return Error::fail_errno(-ret, "io_uring_register");
  }

  // we need to submit a first read, and this is a good place
  if (auto res = ArmWakeRead(); !res) {
    const auto err = res.error();
    ELOGFMT(WARN, "failed to re-arm eventfd read: [{}] {}", err.context(), err.message());
  }
  is_activated_ = true;

  return {};
}

Result<std::size_t> IO::DrainCompletions() noexcept {
  io_uring_cqe* cqe = nullptr;
  unsigned head = 0;
  unsigned count = 0;
  bool needs_rearm = false;

  io_uring_for_each_cqe(&ring_, head, cqe) {
    const uint64_t raw_data = io_uring_cqe_get_data64(cqe);

    if (__builtin_expect(!!(raw_data & 1ULL), 0)) {
      auto* state = reinterpret_cast<MultishotAcceptState*>(raw_data & ~1ULL);

      Result<UniqueFd> client_res;
      if (cqe->res >= 0) {
        client_res = UniqueFd{cqe->res};
      } else {
        client_res = Error::fail_errno(-cqe->res, "multishot_accept");
      }

      // Always invoke the handler: on success, on transient errors (MORE set),
      // and on terminal errors (MORE unset) so the application learns the
      // accept loop ended and can restart or abort.
      state->handler(*this, std::move(client_res));

      if (!(cqe->flags & IORING_CQE_F_MORE)) {
        // Erase from active list
        std::erase(active_multishots_, raw_data);
        delete state;
        --inflight_ops_;
      }
    } else if (raw_data == k_wakeup_sentinel) {
      // Defer re-arm until after cq_advance to avoid submitting a new SQE
      // while the CQ ring is still being iterated.
      needs_rearm = true;
    } else if (raw_data == k_fiber_cancel_sentinel) {
      --inflight_ops_;
    } else if (raw_data != 0) {
      auto* op = static_cast<detail::fiber::Operation*>(io_uring_cqe_get_data(cqe));

      op->cqe_res = cqe->res;
      op->cqe_flags = cqe->flags;

      if (op->fiber) {
        // If a timeout was armed for this op, retire it: this is the
        // common case where the op beat its deadline. O(1) unlink.
        if (op->deadline_ns != 0) {
          timer_wheel_.Disarm(op->fiber);
        }
        PushReady(op->fiber);
        --inflight_ops_;
      }
    }

    count++;
  }

  if (count > 0) {
    io_uring_cq_advance(&ring_, count);
  }

  // Re-arm the wakeup read after advancing the CQ, not mid-iteration.
  if (needs_rearm) {
    if (auto res = ArmWakeRead(); !res) {
      const auto err = res.error();
      ELOGFMT(WARN, "failed to re-arm eventfd read: [{}] {}", err.context(),
              err.message());
    }
  }

  return count;
}

void IO::DrainRemote() {
  constexpr size_t k_max_bulk = 64;
  std::array<std::move_only_function<void(IO&) noexcept>, k_max_bulk> fns;
  size_t dequeued = 0;
  size_t total_processed = 0;

  while (total_processed < opts_.batch_max_size) {
    const size_t to_dequeue =
        std::min(k_max_bulk, opts_.batch_max_size - total_processed);
    dequeued = post_queue_.TryDequeueBulk(fns.data(), to_dequeue);

    if (dequeued == 0) {
      break;
    }

    for (size_t i = 0; i < dequeued; ++i) {
      if (auto res = SpawnTask(std::move(fns[i])); !res) {
        ELOGFMT(ERROR, "failed to spawn posted task: [{}] {}", res.error().context(),
                res.error().message());
      }
    }
    total_processed += dequeued;
  }
}

void IO::ResumeReadyFibers() {
  size_t processed = 0;
  while (processed < opts_.batch_max_size) {
    auto* fiber = PopReady();
    if (!fiber) {
      break;  // empty queue
    }

    // Jump into the user's code
    ResumeFiber(fiber);

    // State Machine Cleanup
    if (fiber->state == detail::fiber::FiberState::done) {
      DestroyFiber(fiber);
    }

    processed++;
  }
}
Result<void> IO::Tick() noexcept {
  if (opts_.loop_style == IoOptions::LoopStyle::kResumeFirst) {
    // ---- kio-style pipeline: resume -> submit -> reap ----
    //
    // Phase 1: harvest tasks posted from remote threads, then resume the
    // ready fibers. Resuming is what arms the SQEs, so doing it first
    // means the submit below carries them into the kernel in this very
    // iteration instead of the next one.
    DrainRemote();
    ResumeReadyFibers();

    // kio's quiesce check. kio verifies that the loop is not about to
    // sleep with zero outstanding operations BEFORE it blocks in
    // submit_and_wait -- otherwise it waits for a completion that can
    // never arrive. The same hazard exists here because phase 1 may have
    // just finished the last fiber.
    if (drain_to_completion_ && !HasLiveFibers() && inflight_ops_ == 0 &&
        ready_queue_.Empty() && post_queue_.Empty()) {
      return {};
    }

    // Phase 2: submit. Sleep only when there is genuinely nothing runnable.
    const bool runnable =
        io_uring_cq_ready(&ring_) > 0 || !ready_queue_.Empty() || !post_queue_.Empty();
    if (auto ret = SubmitOrWait(runnable); !ret) {
      auto err = ret.error();
      ELOGFMT(ERROR, "failed to submit and wait for io: [{}] {}", err.context(),
              err.message());
    }

    // Phase 3: reap. Completions only mark fibers ready; the next
    // iteration's phase 1 resumes them.
    TRY_LOG_ERR(count, DrainCompletions(),
                "current tick run failed to drain completions");
    ELOGFMT(DEBUG, "drained {} io completions", count);

    // Cancel overdue timed ops. Runs after drain_completions() on purpose:
    // ops that completed in this batch have already been disarmed, so we
    // never cancel an op whose result the fiber is about to consume.
    ProcessExpiredTimers();

    return {};
  }

  // Flush SQEs and/or wait for CQEs.
  // If ready_queue_ is empty, this safely drops the CPU to 0% and
  // sleeps the thread until the kernel wakes us via a CQE.
  if (auto ret = SubmitOrWaitFor(); !ret) {
    auto err = ret.error();
    ELOGFMT(ERROR, "failed to submit and wait for io: [{}] {}", err.context(),
            err.message());
  }

  TRY_LOG_ERR(count, DrainCompletions(), "current tick run failed to drain completions");

  ELOGFMT(DEBUG, "drained {} io completions", count);

  // Cancel overdue timed ops. Runs after drain_completions() on purpose:
  // ops that completed in this batch have already been disarmed, so we
  // never cancel an op whose result the fiber is about to consume.
  ProcessExpiredTimers();

  // Drain tasks posted from remote threads and spawn them as fibers.
  // Done here, after drain_completions(), so newly spawned tasks join the
  // ready queue alongside any fibers that were just woken by CQEs.
  // We use try_dequeue_bulk to grab multiple tasks at once efficiently,
  // and limit the total processed per tick to prevent I/O starvation.

  DrainRemote();

  ResumeReadyFibers();

  return {};
}

//
// ======================= Helpers ==========================================
//
void IO::PinToCpu() const {
  const auto size = opts_.worker_cpu_affinity.size();

  if (empty(opts_.worker_cpu_affinity)) {
    return;
  }

  int physical_core_id = opts_.worker_cpu_affinity[id_ % size];
  if (physical_core_id < 0) {
    return;
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<unsigned int>(physical_core_id), &cpuset);

  if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
      rc != 0) {
    ELOGFMT(INFO, "Warning: Failed to pin to physical CPU {}: {}", physical_core_id,
            std::generic_category().message(rc));
  }
}

void IO::Run(const std::stop_token& st) noexcept {
  owner_tid_ = std::this_thread::get_id();
  is_running_ = true;

  PinToCpu();

  if (auto res = Activate(); !res.has_value()) {
    auto err = res.error();
    ELOGFMT(ERROR, "failed to activate io uring: [{}] {}", err.context(), err.message());
    return;
  }

  std::stop_callback wake_on_stop{st, [this] { Wake(); }};
  while (!st.stop_requested()) {
    if (auto res = Tick(); !res) {
      const auto err = res.error();
      ELOGFMT(ERROR, "tick failed: [{}] {}", err.context(), err.message());
    }
  }

  stopping_.store(true, std::memory_order_release);
  ELOGFMT(INFO, "Worker {} quiescing...", id_);
  CancelAndDrain();

  is_running_ = false;

  if (live_fibers_ > 0) {
    ELOGFMT(WARN, "Worker {} stopped with {} live fibers (stuck in app logic?)", id_,
            live_fibers_);
  }
}

void IO::RunUntilDone() noexcept {
  owner_tid_ = std::this_thread::get_id();
  is_running_ = true;

  PinToCpu();

  if (auto res = Activate(); !res) {
    const auto err = res.error();
    ELOGFMT(ERROR, "failed to activate io worker: [{}] {}", err.context(), err.message());
    is_running_ = false;
    return;
  }

  // The loop below decides when to stop, so a tick must never sleep once
  // the runtime is quiesced.
  drain_to_completion_ = true;

  // Also drain the cross-thread post queue: tasks posted before run_until_done()
  // is called would be silently dropped if we only checked live_fibers_.
  // inflight_ops_ tracks user I/O ops and cancel SQEs only (the wake_fd_
  // read is not counted). HasLiveFibers() already covers waiting fibers,
  // but the > 0 check defends against future code paths where an op is
  // in-flight without a corresponding live fiber.
  while (HasLiveFibers() || inflight_ops_ > 0 || !ready_queue_.Empty() ||
         !post_queue_.Empty()) {
    if (auto res = Tick(); !res) {
      const auto err = res.error();
      ELOGFMT(ERROR, "io tick failed: [{}] {}", err.context(), err.message());
      break;
    }
  }

  drain_to_completion_ = false;
  is_running_ = false;
}
//
// ======================= IO Methods ==========================================
//

Result<uint32_t> IO::Read(const UniqueFd& fd, const std::span<std::byte> buf,
                          const off_t offset) noexcept {
  return Syscall("read", [&](io_uring_sqe* sqe) {
    io_uring_prep_read(sqe, fd.Get(), buf.data(), static_cast<unsigned int>(buf.size()),
                       static_cast<std::uint64_t>(offset));
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<uint32_t> IO::ReadFixed(const UniqueFd& fd, FixedBuffer& buf, const size_t len,
                               const off_t file_offset,
                               const size_t buffer_offset) noexcept {
  return Syscall("read_fixed", [&](io_uring_sqe* sqe) {
    void* start_addr = static_cast<std::byte*>(buf.Ptr()) + buffer_offset;
    io_uring_prep_read_fixed(sqe, fd.Get(), start_addr, static_cast<unsigned int>(len),
                             static_cast<std::uint64_t>(file_offset),
                             static_cast<int>(buf.Index()));
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<uint32_t> IO::ReadFixed(const UniqueFd& fd, FixedBuffer& buf,
                               const off_t file_offset) noexcept {
  return ReadFixed(fd, buf, buf.Size(), file_offset, 0);
}

Result<uint32_t> IO::Writev(const UniqueFd& fd, std::span<const iovec> iovecs,
                            const off_t offset) noexcept {
  return Syscall("writev", [&](io_uring_sqe* sqe) {
    io_uring_prep_writev(sqe, fd.Get(), iovecs.data(),
                         static_cast<unsigned int>(iovecs.size()),
                         static_cast<std::uint64_t>(offset));
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<void> IO::Fsync(const UniqueFd& fd, const bool datasync) noexcept {
  return Syscall<void>("fsync", [&](io_uring_sqe* sqe) {
    io_uring_prep_fsync(sqe, fd.Get(), datasync ? IORING_FSYNC_DATASYNC : 0);
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<UniqueFd> IO::Open(const std::filesystem::path& path, const int flags,
                          const mode_t mode) noexcept {
  return Syscall<UniqueFd>("open", [&](io_uring_sqe* sqe) {
    io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode);
  });
}

Result<void> IO::Fallocate(const UniqueFd& fd, const int mode, const off_t offset,
                           const off_t len) noexcept {
  return Syscall<void>("fallocate", [&](io_uring_sqe* sqe) {
    io_uring_prep_fallocate(sqe, fd.Get(), mode, static_cast<std::uint64_t>(offset),
                            static_cast<std::uint64_t>(len));
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<void> IO::Ftruncate(const UniqueFd& fd, const off_t len) noexcept {
  return Syscall<void>("ftruncate", [&](io_uring_sqe* sqe) {
    io_uring_prep_ftruncate(sqe, fd.Get(), len);
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<UniqueFd> IO::Accept(const UniqueFd& server_fd, SocketAddress& client_addr,
                            int flags) noexcept {
  return Syscall<UniqueFd>("accept", [&](io_uring_sqe* sqe) {
    client_addr.addrlen = sizeof(sockaddr_storage);
    io_uring_prep_accept(sqe, server_fd.Get(), client_addr.GetMutable(),
                         &client_addr.addrlen, flags);
    if (server_fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<UniqueFd> IO::Accept(const UniqueFd& server_fd, const int flags) noexcept {
  return Syscall<UniqueFd>("accept", [&](io_uring_sqe* sqe) {
    io_uring_prep_accept(sqe, server_fd.Get(), nullptr, nullptr, flags);
    if (server_fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<uint32_t> IO::Write(const UniqueFd& fd, std::span<const std::byte> buf,
                           const off_t offset) noexcept {
  return Syscall("write", [&](io_uring_sqe* sqe) {
    io_uring_prep_write(sqe, fd.Get(), buf.data(), static_cast<unsigned int>(buf.size()),
                        static_cast<std::uint64_t>(offset));
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<uint32_t> IO::WriteFixed(const UniqueFd& fd, const FixedBuffer& buf,
                                const size_t len, const off_t file_offset,
                                const size_t buffer_offset) noexcept {
  return Syscall("write_fixed", [&](io_uring_sqe* sqe) {
    const void* start_addr = buf.Ptr() + buffer_offset;
    io_uring_prep_write_fixed(sqe, fd.Get(), start_addr, static_cast<unsigned int>(len),
                              static_cast<std::uint64_t>(file_offset),
                              static_cast<int>(buf.Index()));
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<uint32_t> IO::WriteFixed(const UniqueFd& fd, const FixedBuffer& buf,
                                const off_t file_offset) noexcept {
  return WriteFixed(fd, buf, buf.Size(), file_offset, 0);
}

Result<uint32_t> IO::Readv(const UniqueFd& fd, std::span<const iovec> iovecs,
                           const off_t offset) noexcept {
  return Syscall("readv", [&](io_uring_sqe* sqe) {
    io_uring_prep_readv(sqe, fd.Get(), iovecs.data(),
                        static_cast<unsigned int>(iovecs.size()),
                        static_cast<std::uint64_t>(offset));
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<void> IO::Remove(const std::filesystem::path& path) noexcept {
  return Syscall<void>("remove", [&](io_uring_sqe* sqe) {
    io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0);
  });
}

Result<void> IO::Rename(const std::filesystem::path& from,
                        const std::filesystem::path& to) noexcept {
  return Syscall<void>("rename", [&](io_uring_sqe* sqe) {
    io_uring_prep_renameat(sqe, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0);
  });
}

Result<uint32_t> IO::Poll(const UniqueFd& fd, const unsigned poll_mask) noexcept {
  return Syscall("poll", [&](io_uring_sqe* sqe) {
    io_uring_prep_poll_add(sqe, fd.Get(), poll_mask);
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<void> IO::Connect(const UniqueFd& fd, const SocketAddress& addr) noexcept {
  return Syscall<void>("connect", [&](io_uring_sqe* sqe) {
    io_uring_prep_connect(sqe, fd.Get(), addr.Get(), addr.addrlen);
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

void IO::ProcessExpiredTimers() noexcept {
  if (!timer_wheel_.HasArmed()) {
    return;
  }

  bool issued = false;
  timer_wheel_.Expire(
      detail::MonotonicNowNs(), [&](detail::fiber::Fiber* fiber) noexcept {
        auto res = GetSqe();
        if (!res) {
          // Could not cancel now: re-arm one tick out and retry.
          ELOGFMT(ERROR, "timeout cancel SQE alloc failed: [{}] {}",
                  res.error().context(), res.error().message());
          timer_wheel_.Arm(fiber,
                           detail::MonotonicNowNs() + detail::TimerWheel::k_tick_ns);
          return;
        }
        fiber->operation.timed_out = true;
        io_uring_prep_cancel64(res.value(), reinterpret_cast<__u64>(&fiber->operation),
                               0);
        io_uring_sqe_set_data64(res.value(), k_fiber_cancel_sentinel);
        ++inflight_ops_;
        issued = true;
      });

  if (issued) {
    // Submit immediately (rare path, cheap insurance): the cancel must
    // reach the kernel before any future SQE that reuses this fiber's
    // &Operation as user_data, so it can never match a successor op.
    io_uring_submit(&ring_);
  }
}

Result<uint32_t> IO::Read(const UniqueFd& fd, const std::span<std::byte> buf,
                          const std::chrono::nanoseconds timeout,
                          const off_t offset) noexcept {
  return SyscallTimed("read_timed", timeout, [&](io_uring_sqe* sqe) {
    io_uring_prep_read(sqe, fd.Get(), buf.data(), static_cast<unsigned int>(buf.size()),
                       static_cast<std::uint64_t>(offset));
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<uint32_t> IO::Write(const UniqueFd& fd, const std::span<const std::byte> buf,
                           const std::chrono::nanoseconds timeout,
                           const off_t offset) noexcept {
  return SyscallTimed("write_timed", timeout, [&](io_uring_sqe* sqe) {
    io_uring_prep_write(sqe, fd.Get(), buf.data(), static_cast<unsigned int>(buf.size()),
                        static_cast<std::uint64_t>(offset));
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<UniqueFd> IO::Accept(const UniqueFd& server_fd,
                            const std::chrono::nanoseconds timeout,
                            const int flags) noexcept {
  return SyscallTimed<UniqueFd>("accept_timed", timeout, [&](io_uring_sqe* sqe) {
    io_uring_prep_accept(sqe, server_fd.Get(), nullptr, nullptr, flags);
    if (server_fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<void> IO::Connect(const UniqueFd& fd, const SocketAddress& addr,
                         const std::chrono::nanoseconds timeout) noexcept {
  return SyscallTimed<void>("connect_timed", timeout, [&](io_uring_sqe* sqe) {
    io_uring_prep_connect(sqe, fd.Get(), addr.Get(), addr.addrlen);
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<uint32_t> IO::Poll(const UniqueFd& fd, const unsigned poll_mask,
                          const std::chrono::nanoseconds timeout) noexcept {
  return SyscallTimed("poll_timed", timeout, [&](io_uring_sqe* sqe) {
    io_uring_prep_poll_add(sqe, fd.Get(), poll_mask);
    if (fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;
  });
}

Result<void> IO::SleepForNs(const std::chrono::nanoseconds duration) noexcept {
  if (duration.count() <= 0) {
    return {};
  }

  __kernel_timespec ts;
  const auto ns = duration.count();
  ts.tv_sec = ns / 1'000'000'000LL;
  ts.tv_nsec = ns % 1'000'000'000LL;

  // Both structs have two 64-bit signed integers (tv_sec, tv_nsec) on 64-bit systems.
  static_assert(sizeof(struct timespec) == sizeof(__kernel_timespec),
                "POSIX timespec and __kernel_timespec size mismatch");

  auto r = Syscall<void>(
      "sleep_for", [&](io_uring_sqe* sqe) { io_uring_prep_timeout(sqe, &ts, 0U, 0U); });

  if (!r && r.error().value() == ETIME) {
    return {};
  }

  return r;
}

Result<void> IO::AcceptLoopImpl(
    UniqueFd server_fd,
    std::move_only_function<void(IO&, Result<UniqueFd>) noexcept> handler) noexcept {
  if (stopping_) return Error::fail_errno(ECANCELED, "accept_loop");
  if (!server_fd.Valid()) return Error::fail_errno(EBADF, "accept_loop: invalid fd");

  auto* state = new (std::nothrow) MultishotAcceptState{.server_fd = std::move(server_fd),
                                                        .handler = std::move(handler)};
  if (!state) return Error::fail_errno(ENOMEM, "accept_loop: OOM allocating state");
  static_assert(alignof(MultishotAcceptState) >= 2,
                "tagged pointer requires even alignment");

  auto* sqe = GetSqe().value_or(nullptr);
  if (!sqe) {
    delete state;
    return Error::fail_errno(EAGAIN, "accept_loop: ring full");
  }

  io_uring_prep_multishot_accept(sqe, state->server_fd.Get(), nullptr, nullptr, 0);
  if (state->server_fd.IsDirect()) sqe->flags |= IOSQE_FIXED_FILE;

  // Tag the lowest bit to indicate a MultishotAcceptState
  const uint64_t tagged_ptr = reinterpret_cast<uint64_t>(state) | 1ULL;
  io_uring_sqe_set_data64(sqe, tagged_ptr);
  active_multishots_.push_back(tagged_ptr);

  inflight_ops_++;
  return {};
}

}  // namespace tinyio
