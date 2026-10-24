#include "runtime.hpp"

#include <sched.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include <sys/eventfd.h>
#include <ylt/easylog.hpp>

#include "check.hpp"

namespace tinyio {
IO::IO(const size_t id, const IoOptions& opts)
    : id_(id), opts_(opts), post_queue_(opts.post_queue_capacity) {
  if (opts.buffer_pool) {
    buffer_pool_.emplace(opts.buffer_pool->slot_size, opts.buffer_pool->slots);
    buffer_pool_->SetOwner(this);
  }

  if (auto res = Initialize(); !res) {
    // The destructor does not run when a constructor throws: release what
    // Initialize() created before it failed.
    if (wake_fd_ >= 0) {
      ::close(wake_fd_);
      wake_fd_ = -1;
    }
    if (ring_.ring_fd > 0) {
      io_uring_queue_exit(&ring_);
      ring_.ring_fd = -1;
    }
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

  // Only a runtime that never ran gets here with live fibers (spawned, never
  // started): a drain aborts on fibers that are still parked.
  if (live_fibers_ > 0) {
    ELOGFMT(WARN, "IO #{} teardown: {} fiber(s) never ran", id_, live_fibers_);
  }

  // The constructor throws when setup fails, so the ring and the eventfd
  // always exist here.
  io_uring_queue_exit(&ring_);
  ELOGFMT(INFO, "exited io-uring ring, worker {}", id_);
  {
    // Take the same lock Wake() uses: a poster may still be inside write().
    std::lock_guard<std::mutex> lock(wake_mutex_);
    ::close(wake_fd_);
    wake_fd_ = -1;
  }

  // Detach the pool before the fibers go away: a fiber parked on a slot
  // points at memory that is about to be freed, so the pool must not try to
  // unpark it afterwards.
  if (buffer_pool_) {
    buffer_pool_->Detach();
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

  // The kernel rejects this pair with a bare EINVAL; say why instead.
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
    return Error::fail_errno(errno, "eventfd");
  }

  // One iovec per <= 1 GiB chunk, not per slot: the kernel's registered
  // buffer table is far smaller than the number of slots a pool holds. The
  // kernel pins the pages, which counts against RLIMIT_MEMLOCK unless the
  // process has CAP_IPC_LOCK.
  if (buffer_pool_) {
    std::vector<iovec> table;
    buffer_pool_->AppendRegions(table);
    if (const int ret = io_uring_register_buffers(
            &ring_, table.data(), static_cast<unsigned int>(table.size()));
        ret < 0) {
      ELOGFMT(ERROR,
              "io_uring_register_buffers failed: {} ({} regions, {} bytes pinned){}",
              std::strerror(-ret), table.size(), buffer_pool_->AllocatedBytes(),
              ret == -ENOMEM ? "; check `ulimit -l` (RLIMIT_MEMLOCK) or LimitMEMLOCK= "
                               "in the unit file"
                             : "");
      return Error::fail_errno(-ret, "io_uring_register_buffers");
    }
    ELOGFMT(INFO, "registered a buffer pool of {} x {} bytes", buffer_pool_->Capacity(),
            buffer_pool_->SlotSize());
  }

  ELOGFMT(INFO, "Initialized IO context with {} submission entries (SQPOLL: {})",
          opts_.sq_entries,
          (opts_.additional_uring_flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");

  return {};
}

Result<FixedBuffer> IO::TakeFixedBuffer() noexcept {
  CHECK(buffer_pool_.has_value())
      << "take_fixed_buffer: no buffer pool configured (IoOptions::buffer_pool)";
  return buffer_pool_->Take();
}

Result<FixedBuffer> IO::AcquireFixedBuffer() noexcept {
  CHECK(buffer_pool_.has_value())
      << "acquire_fixed_buffer: no buffer pool configured (IoOptions::buffer_pool)";
  return buffer_pool_->Acquire(*this);
}

Result<unsigned int> IO::FixedRange(const FixedBuffer& buf, const size_t len,
                                    const size_t buffer_offset,
                                    const char* op_name) const noexcept {
  // The slot has to come from this runtime's pool: the kernel resolves the
  // buf_index against the table registered with this ring, and a foreign
  // index would name the wrong buffer.
  CHECK(buffer_pool_.has_value() && buf.Pool() == &*buffer_pool_)
      << op_name << ": the buffer is not registered with this runtime";
  // The buffer offset has to land inside the slot, and the length must not
  // be zero: either is an EINVAL. A length that runs past the slot is
  // clamped to what is left of it, so the kernel never touches the next
  // slot.
  if (buffer_offset >= buf.Size() || len == 0) {
    return Error::fail_errno(EINVAL, op_name);
  }
  return static_cast<unsigned int>(std::min({len, buf.Size() - buffer_offset, kMaxRwLen}));
}

Result<void> IO::Close(const int fd) noexcept {
  return Syscall<void>("close", [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd); });
}

Result<void> IO::Close(UniqueFd fd) noexcept {
  if (!fd.Valid()) return {};

  return Syscall<void>(
      "close", [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd.Release()); });
}

//
// Fiber management
//
void IO::PushReady(detail::fiber::Fiber* fiber) noexcept {
  assert(fiber != nullptr && fiber->state != detail::fiber::FiberState::done);
  // Ready means "in the ready queue": a fiber is queued at most once, however
  // many wakeups it got.
  if (fiber->state == detail::fiber::FiberState::ready) {
    return;
  }
  fiber->state = detail::fiber::FiberState::ready;
  ready_queue_.Push(fiber);
}

detail::fiber::Fiber* IO::PopReady() noexcept {
  detail::fiber::Fiber* fiber = nullptr;
  (void)ready_queue_.Pop(fiber);
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
  // A fiber finishes while it runs: it is out of the ready queue, and its
  // last operation completed, which took it out of the timer wheel.
  assert(fiber->state == detail::fiber::FiberState::done && !fiber->timer_armed);
  fiber->task = nullptr;

  detail::ToolsDestroyFiber(fiber->tools);

  if (idle_fibers_.size() >= opts_.max_idle_fibers) {
    // Over the cache limit: give the stack back to the OS.
    RemoveFromAllFibers(fiber);
    detail::stack::ReleaseStack(fiber->stack);
    delete fiber;
  } else {
    // Keep it, stack included, for the next spawn. It stays in all_fibers_.
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
    // unique_ptr frees the Fiber if the stack allocation fails, and the
    // fiber joins all_fibers_ only once both allocations succeeded.
    std::unique_ptr<detail::fiber::Fiber> owned(new (std::nothrow)
                                                    detail::fiber::Fiber());
    if (!owned) {
      return Error::fail_errno(ENOMEM, "spawn");
    }
    TRY(const stack, detail::stack::AllocateStack(opts_.stack_size, opts_.guard_size));
    owned->runtime = this;
    owned->stack = stack;
    owned->all_fibers_index = all_fibers_.size();
    all_fibers_.push_back(owned.get());
    fiber = owned.release();
  }

  ++live_fibers_;

  // Every execution gets a fresh id, a recycled fiber included.
  fiber->id = next_fiber_id_++;
  fiber->task = std::move(task);
  detail::ToolsCreateFiber(fiber->tools);
  detail::context::Make(fiber->context, fiber->stack.high, &IO::FiberEntry, fiber);

  // A new or recycled fiber is in no queue yet.
  fiber->state = detail::fiber::FiberState::ready;
  ready_queue_.Push(fiber);

  return fiber->id;
}

TIO_NO_SANITIZE_ADDRESS void IO::ResumeFiber(detail::fiber::Fiber* fiber) noexcept {
  current_fiber_ = fiber;
  fiber->state = detail::fiber::FiberState::running;

  detail::ToolsBeforeSwitch(scheduler_tools_, &fiber->tools, &fiber->stack);
  detail::context::Switch(scheduler_context_, fiber->context);
  detail::ToolsAfterSwitch(scheduler_tools_);

  // Back on the scheduler.
  current_fiber_ = nullptr;
}

TIO_NO_SANITIZE_ADDRESS void IO::SuspendFiber(detail::fiber::Fiber* fiber) noexcept {
  detail::ToolsBeforeSwitch(fiber->tools, &scheduler_tools_, nullptr);
  detail::context::Switch(fiber->context, scheduler_context_);
  detail::ToolsAfterSwitch(fiber->tools);
}

[[nodiscard]] Result<void> IO::Yield() noexcept {
  CheckCanSubmit("yield");

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

// First code a new fiber runs, on its own stack, reached through the
// trampoline in context.cc. ResumeFiber() saved the scheduler's context in
// the switch that got us here, so there is nothing to record.
[[noreturn]] void IO::FiberEntry(void* arg) noexcept {
  auto* fiber = static_cast<detail::fiber::Fiber*>(arg);
  assert(fiber != nullptr && fiber->runtime != nullptr);

  // Complete the ASAN fiber switch ResumeFiber() started. A brand-new fiber
  // has no fake stack to restore: fiber->tools.asan_fake_stack is still the
  // nullptr ToolsCreateFiber() set.
  detail::ToolsAfterSwitch(fiber->tools);
  fiber->task(*fiber->runtime);
  fiber->runtime->FinishCurrentFiber(fiber);
}

//
// Shutdown
//

void IO::CancelAndDrain() noexcept {
  // Cancel every in-flight request with ONE SQE.
  //
  // IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY matches every request
  // in the ring, whatever its user_data: at this point every request in the
  // ring belongs to the runtime, and every one of them has to die. The
  // wake-read is in the ring too; DrainCompletions() re-arms it, so
  // cancelling it costs one extra completion and nothing else.
  //
  // Sync-parked fibers are never woken by the runtime. They might be waiting
  // for the IO fibers to finish (e.g., TaskGroup::Wait, Latch::Wait), and
  // waking them early would let them destroy synchronization primitives
  // while the IO fibers are still unwinding, causing Use-After-Free.
  if (inflight_ops_ > 0) {
    if (auto sqe = GetSqe(); sqe) {
      io_uring_prep_cancel64(*sqe, 0, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY);
      io_uring_sqe_set_data64(*sqe, k_fiber_cancel_sentinel);
      ++inflight_ops_;
      io_uring_submit(&ring_);
    } else {
      ELOGFMT(ERROR, "cancel SQE alloc failed: [{}] {}", sqe.error().context(),
              sqe.error().message());
    }
  }

  // The cancelled IO fibers wake with ECANCELED and unwind, which lets them
  // wake their sync-parked dependents naturally.
  DriveToQuiescence("cancel_and_drain");
  AbortIfParkedFibers();
}

bool IO::TickCounted(int& consecutive_errors, const char* who) noexcept {
  auto res = Tick();
  if (res) {
    consecutive_errors = 0;
    return true;
  }
  ELOGFMT(ERROR, "{}: tick failed: [{}] {}", who, res.error().context(),
          res.error().message());
  return ++consecutive_errors < k_max_tick_errors;
}

void IO::DriveToQuiescence(const char* who) noexcept {
  // This loop decides when to stop, so a tick must not sleep once the
  // runtime is quiesced: it would wait for a completion that can never come.
  drain_to_completion_ = true;
  int consecutive_errors = 0;
  while (inflight_ops_ > 0 || !ready_queue_.Empty() || !post_queue_.Empty()) {
    // Giving up must not return: operations would still be in flight when
    // the ring is torn down, and the kernel would then reference Operation
    // objects that live inside the fibers about to be freed.
    if (!TickCounted(consecutive_errors, who)) {
      ELOGFMT(FATAL,
              "IO #{} {}: {} operation(s) still in flight after {} failed ticks; "
              "tearing the ring down now would be a kernel use-after-free",
              id_, who, inflight_ops_, consecutive_errors);
      std::abort();
    }
  }
  drain_to_completion_ = false;
}

void IO::AbortIfParkedFibers() const noexcept {
  // Called once quiesced: nothing ready, nothing in flight. A fiber still
  // live is parked on a sync primitive or on the buffer pool, and nothing
  // can ever wake it.
  if (live_fibers_ == 0) {
    return;
  }

  // Name the culprit: every parked fiber records what it is waiting on, so
  // the report says "1 on Mutex (fiber 4)" instead of a count plus guesses.
  // Grouped by WaitKind, in enum order.
  constexpr auto k_kinds = static_cast<size_t>(detail::fiber::WaitKind::buffer_pool) + 1;
  std::array<std::string, k_kinds> ids;
  std::array<size_t, k_kinds> counts{};
  for (const detail::fiber::Fiber* fiber : all_fibers_) {
    if (fiber->state != detail::fiber::FiberState::waiting) {
      continue;
    }
    const auto kind = static_cast<size_t>(fiber->wait_kind);
    ids[kind] += (counts[kind]++ == 0 ? "" : ", ") + std::to_string(fiber->id);
  }

  std::string report;
  for (size_t kind = 0; kind < k_kinds; ++kind) {
    if (counts[kind] == 0) {
      continue;
    }
    report += std::format(
        "{}{} on {} (fiber{} {})", report.empty() ? "" : ", ", counts[kind],
        detail::fiber::WaitKindName(static_cast<detail::fiber::WaitKind>(kind)),
        counts[kind] == 1 ? "" : "s", ids[kind]);
  }

  ELOGFMT(FATAL,
          "IO #{} quiesced with {} parked fiber(s) and no one left to wake them: {}", id_,
          live_fibers_, report.empty() ? "unknown" : report);
  std::abort();
}

//
// The event loop
//

Result<void> IO::Tick() noexcept {
  // resume -> submit/wait -> reap (kio's pipeline).
  //
  // Phase 1: harvest tasks posted from remote threads, then resume the
  // ready fibers. Resuming is what arms the SQEs, so doing it first means
  // the submit below carries them into the kernel in this very iteration.
  DrainRemote();
  ResumeReadyFibers();

  // Quiesce check: phase 1 may just have finished the last fiber, and
  // blocking in submit_and_wait now would wait for a completion that can
  // never arrive. Live fibers do not count: one parked on a sync primitive
  // waits for no completion either.
  if (drain_to_completion_ && inflight_ops_ == 0 && ready_queue_.Empty() &&
      post_queue_.Empty()) {
    return {};
  }

  // Phase 2: submit, and sleep only when nothing is runnable.
  auto submitted = SubmitOrWait();

  // Phase 3: reap, even when the submit failed: completions already in the
  // CQ still have to wake their fibers. They only mark fibers ready; the
  // next iteration's phase 1 resumes them.
  const std::size_t count = DrainCompletions();
  ELOGFMT(DEBUG, "drained {} io completions", count);

  // Cancel overdue timed ops. Runs after DrainCompletions() on purpose:
  // ops that completed in this batch have already been disarmed, so we
  // never cancel an op whose result the fiber is about to consume.
  ProcessExpiredTimers();

  return submitted;
}

void IO::DrainRemote() {
  constexpr size_t k_max_bulk = 64;
  std::array<std::move_only_function<void(IO&) noexcept>, k_max_bulk> fns;

  // Bounded per tick, so a flood of posts cannot starve the I/O.
  size_t total = 0;
  while (total < opts_.batch_max_size) {
    const size_t dequeued = post_queue_.TryDequeueBulk(
        fns.data(), std::min(k_max_bulk, opts_.batch_max_size - total));
    if (dequeued == 0) {
      break;
    }

    for (size_t i = 0; i < dequeued; ++i) {
      if (auto res = SpawnTask(std::move(fns[i])); !res) {
        ELOGFMT(ERROR, "failed to spawn posted task: [{}] {}", res.error().context(),
                res.error().message());
      }
    }
    total += dequeued;
  }
}

void IO::ResumeReadyFibers() {
  for (size_t processed = 0; processed < opts_.batch_max_size; ++processed) {
    auto* fiber = PopReady();
    if (fiber == nullptr) {
      break;
    }

    ResumeFiber(fiber);

    if (fiber->state == detail::fiber::FiberState::done) {
      DestroyFiber(fiber);
    }
  }
}

Result<void> IO::SubmitOrWait() noexcept {
  // Only block when there is no local work to do.
  const bool runnable =
      io_uring_cq_ready(&ring_) > 0 || !ready_queue_.Empty() || !post_queue_.Empty();

  if (runnable) {
    const bool get_events =
        (opts_.additional_uring_flags & IORING_SETUP_DEFER_TASKRUN) != 0;
    if (const int ret = SubmitSqes(get_events); ret < 0) {
      return Error::fail_errno(-ret, "submit_or_wait.io_uring_submit");
    }
    return {};
  }

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
  const auto ret = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, tsp, nullptr);
  if (ret >= 0 || ret == -EINTR || ret == -ETIME) {
    return {};
  }
  // The same transient failures SubmitSqes() survives, and this is the
  // GETEVENTS path the man page names for EBUSY. Reap so the kernel can
  // flush its overflow, and let the next tick wait again.
  if (ret == -EBUSY || ret == -EAGAIN) {
    ReapOverflow();
    return {};
  }
  return Error::fail_errno(-ret, "submit_or_wait.io_uring_submit_and_wait_timeout");
}

int IO::SubmitSqes(const bool get_events) noexcept {
  // Two failures are transient and the requests stay queued:
  //
  //   -EINTR            the syscall was interrupted before it did anything.
  //   -EBUSY/-EAGAIN    the kernel could not flush the completion queue:
  //                      the CQ ring is full and the overflow list could not
  //                      be extended. The documented recovery is to reap the
  //                      completions and submit again.
  bool reaped = false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const int ret =
        get_events ? io_uring_submit_and_get_events(&ring_) : io_uring_submit(&ring_);
    if (ret >= 0) {
      return ret;
    }
    if (ret == -EINTR) {
      continue;
    }
    if ((ret == -EBUSY || ret == -EAGAIN) && !reaped) {
      reaped = true;
      ReapOverflow();
      continue;
    }
    return ret;
  }
  return -EAGAIN;
}

void IO::ReapOverflow() noexcept {
  // Flush the kernel-side overflow into the CQ, then reap it so the CQ head
  // advances and the kernel has room for the next completion. Reaping only
  // marks fibers ready, so it is safe inside a fiber's GetSqe() too.
  (void)io_uring_get_events(&ring_);
  (void)DrainCompletions();
}

std::size_t IO::DrainCompletions() noexcept {
  io_uring_cqe* cqe = nullptr;
  unsigned head = 0;
  unsigned count = 0;
  bool needs_rearm = false;

  io_uring_for_each_cqe(&ring_, head, cqe) {
    ++count;
    const uint64_t data = io_uring_cqe_get_data64(cqe);

    if (data == k_wakeup_sentinel) {
      // Re-armed after cq_advance: no new SQE while the CQ is iterated.
      needs_rearm = true;
      continue;
    }

    --inflight_ops_;
    if (data == k_fiber_cancel_sentinel) {
      continue;
    }

    // Anything else is the Operation embedded in the fiber that submitted it.
    auto* op = reinterpret_cast<detail::fiber::Operation*>(data);
    op->cqe_res = cqe->res;
    // The common case of a timed op beating its deadline. O(1) unlink.
    timer_wheel_.Disarm(op->fiber);
    PushReady(op->fiber);
  }

  io_uring_cq_advance(&ring_, count);

  if (needs_rearm) {
    if (auto res = ArmWakeRead(); !res) {
      ELOGFMT(WARN, "failed to re-arm eventfd read: [{}] {}", res.error().context(),
              res.error().message());
    }
  }

  return count;
}

Result<io_uring_sqe*> IO::GetSqe() noexcept {
  io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (sqe == nullptr) {
    // The SQ is full: submit what is queued so the kernel can consume the
    // entries. Reaping would not help here: it frees CQ space, not SQ slots.
    if (const int ret = SubmitSqes(false); ret < 0) {
      return Error::fail_errno(-ret, "get_sqe.submit");
    }
    sqe = io_uring_get_sqe(&ring_);
  }

  if (sqe == nullptr) {
    return Error::fail_errno(ENOSPC, "get_sqe");
  }
  return sqe;
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

//
// Cross-thread wakeup
//

void IO::Wake() noexcept {
  constexpr uint64_t one = 1;
  // The lock keeps the write from racing with the destructor's close().
  std::lock_guard<std::mutex> lock(wake_mutex_);
  if (wake_fd_ < 0) {
    // The IO is being torn down; there is nobody left to wake up.
    return;
  }
  // A non-blocking eventfd write never sleeps, so it cannot be interrupted.
  // EAGAIN means the counter is saturated, which already wakes the reader.
  if (::write(wake_fd_, &one, sizeof(one)) < 0 && errno != EAGAIN) {
    ELOGFMT(WARN, "failed to wake io context with eventfd: {}", std::strerror(errno));
  }
}

Result<void> IO::ArmWakeRead() noexcept {
  TRY(sqe, GetSqe());
  io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
  io_uring_sqe_set_data64(sqe, k_wakeup_sentinel);
  io_uring_submit(&ring_);
  return {};
}

//
// Running
//

Result<void> IO::Activate() noexcept {
  // The thread that runs the loop owns the ring, whichever thread built it.
  owner_tid_ = std::this_thread::get_id();
  PinToCpu();

  if (is_activated_) {
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

  // The eventfd read that lets Wake() interrupt a sleeping loop.
  if (auto res = ArmWakeRead(); !res) {
    ELOGFMT(WARN, "failed to arm eventfd read: [{}] {}", res.error().context(),
            res.error().message());
  }
  is_activated_ = true;

  return {};
}

void IO::PinToCpu() const {
  const auto& cpus = opts_.worker_cpu_affinity;
  if (cpus.empty()) {
    return;
  }

  const int cpu = cpus[id_ % cpus.size()];
  if (cpu < 0) {
    return;
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<unsigned int>(cpu), &cpuset);

  if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
      rc != 0) {
    ELOGFMT(INFO, "Warning: Failed to pin to physical CPU {}: {}", cpu,
            std::generic_category().message(rc));
  }
}

void IO::Run(const std::stop_token& st) noexcept {
  if (auto res = Activate(); !res) {
    ELOGFMT(ERROR, "failed to activate io uring: [{}] {}", res.error().context(),
            res.error().message());
    return;
  }

  std::stop_callback wake_on_stop{st, [this] { Wake(); }};
  // A tick that keeps failing must not spin the loop forever: stop after a
  // run of consecutive failures and let the drain below empty the ring.
  int consecutive_errors = 0;
  while (!st.stop_requested()) {
    if (!TickCounted(consecutive_errors, "run")) {
      ELOGFMT(ERROR, "{} consecutive tick failures; stopping the loop to drain",
              consecutive_errors);
      break;
    }
  }

  stopping_.store(true, std::memory_order_release);
  ELOGFMT(INFO, "Worker {} quiescing...", id_);
  CancelAndDrain();
}

void IO::RunUntilDone() noexcept {
  if (auto res = Activate(); !res) {
    ELOGFMT(ERROR, "failed to activate io worker: [{}] {}", res.error().context(),
            res.error().message());
    return;
  }

  // Run until quiesced: nothing ready, nothing in flight, nothing posted.
  // inflight_ops_ counts I/O ops and cancel SQEs only (the wake_fd_ read is
  // not counted).
  DriveToQuiescence("run_until_done");
  AbortIfParkedFibers();
}

//
// ======================= IO Methods ==========================================
//

Result<uint32_t> IO::Read(const UniqueFd& fd, const std::span<std::byte> buf,
                          const off_t offset,
                          const std::chrono::nanoseconds timeout) noexcept {
  // A span past the kernel's per-op cap would truncate through the unsigned
  // SQE length; clamp so the caller gets a short read instead of a wrong one.
  const auto len = static_cast<unsigned int>(std::min(buf.size(), kMaxRwLen));
  return Syscall(
      "read",
      [&](io_uring_sqe* sqe) {
        io_uring_prep_read(sqe, fd.Get(), buf.data(), len,
                           static_cast<std::uint64_t>(offset));
      },
      timeout);
}

Result<uint32_t> IO::Write(const UniqueFd& fd, const std::span<const std::byte> buf,
                           const off_t offset,
                           const std::chrono::nanoseconds timeout) noexcept {
  // Same clamp as Read: a partial write beats a truncated length.
  const auto len = static_cast<unsigned int>(std::min(buf.size(), kMaxRwLen));
  return Syscall(
      "write",
      [&](io_uring_sqe* sqe) {
        io_uring_prep_write(sqe, fd.Get(), buf.data(), len,
                            static_cast<std::uint64_t>(offset));
      },
      timeout);
}

Result<uint32_t> IO::ReadFixed(const UniqueFd& fd, FixedBuffer& buf, const size_t len,
                               const off_t file_offset,
                               const size_t buffer_offset) noexcept {
  TRY(const safe_len, FixedRange(buf, len, buffer_offset, "read_fixed"));
  auto* start_addr = buf.Ptr() + buffer_offset;
  return Syscall("read_fixed", [&](io_uring_sqe* sqe) {
    io_uring_prep_read_fixed(sqe, fd.Get(), start_addr, safe_len,
                             static_cast<std::uint64_t>(file_offset),
                             static_cast<int>(buf.Index()));
  });
}

Result<uint32_t> IO::ReadFixed(const UniqueFd& fd, FixedBuffer& buf,
                               const off_t file_offset) noexcept {
  return ReadFixed(fd, buf, buf.Size(), file_offset, 0);
}

Result<uint32_t> IO::WriteFixed(const UniqueFd& fd, const FixedBuffer& buf,
                                const size_t len, const off_t file_offset,
                                const size_t buffer_offset) noexcept {
  TRY(const safe_len, FixedRange(buf, len, buffer_offset, "write_fixed"));
  const auto* start_addr = buf.Ptr() + buffer_offset;
  return Syscall("write_fixed", [&](io_uring_sqe* sqe) {
    io_uring_prep_write_fixed(sqe, fd.Get(), start_addr, safe_len,
                              static_cast<std::uint64_t>(file_offset),
                              static_cast<int>(buf.Index()));
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
  });
}

Result<uint32_t> IO::Writev(const UniqueFd& fd, std::span<const iovec> iovecs,
                            const off_t offset) noexcept {
  return Syscall("writev", [&](io_uring_sqe* sqe) {
    io_uring_prep_writev(sqe, fd.Get(), iovecs.data(),
                         static_cast<unsigned int>(iovecs.size()),
                         static_cast<std::uint64_t>(offset));
  });
}

Result<void> IO::Fsync(const UniqueFd& fd, const bool datasync) noexcept {
  return Syscall<void>("fsync", [&](io_uring_sqe* sqe) {
    io_uring_prep_fsync(sqe, fd.Get(), datasync ? IORING_FSYNC_DATASYNC : 0);
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
  });
}

Result<void> IO::Ftruncate(const UniqueFd& fd, const off_t len) noexcept {
  return Syscall<void>("ftruncate", [&](io_uring_sqe* sqe) {
    io_uring_prep_ftruncate(sqe, fd.Get(), len);
  });
}

Result<UniqueFd> IO::Accept(const UniqueFd& server_fd, SocketAddress& client_addr,
                            const int flags,
                            const std::chrono::nanoseconds timeout) noexcept {
  return Syscall<UniqueFd>(
      "accept",
      [&](io_uring_sqe* sqe) {
        client_addr.addrlen = sizeof(sockaddr_storage);
        io_uring_prep_accept(sqe, server_fd.Get(), client_addr.GetMutable(),
                             &client_addr.addrlen, flags);
      },
      timeout);
}

Result<UniqueFd> IO::Accept(const UniqueFd& server_fd, const int flags,
                            const std::chrono::nanoseconds timeout) noexcept {
  return Syscall<UniqueFd>(
      "accept",
      [&](io_uring_sqe* sqe) {
        io_uring_prep_accept(sqe, server_fd.Get(), nullptr, nullptr, flags);
      },
      timeout);
}

Result<void> IO::Connect(const UniqueFd& fd, const SocketAddress& addr,
                         const std::chrono::nanoseconds timeout) noexcept {
  return Syscall<void>(
      "connect",
      [&](io_uring_sqe* sqe) {
        io_uring_prep_connect(sqe, fd.Get(), addr.Get(), addr.addrlen);
      },
      timeout);
}

Result<uint32_t> IO::Poll(const UniqueFd& fd, const unsigned poll_mask,
                          const std::chrono::nanoseconds timeout) noexcept {
  return Syscall(
      "poll",
      [&](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, fd.Get(), poll_mask); },
      timeout);
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

Result<void> IO::SleepForNs(const std::chrono::nanoseconds duration) noexcept {
  if (duration.count() <= 0) {
    return {};
  }

  __kernel_timespec ts;
  const auto ns = duration.count();
  ts.tv_sec = ns / 1'000'000'000LL;
  ts.tv_nsec = ns % 1'000'000'000LL;

  // A timeout SQE completes with -ETIME when it fires: that is success here.
  auto r = Syscall<void>(
      "sleep_for", [&](io_uring_sqe* sqe) { io_uring_prep_timeout(sqe, &ts, 0U, 0U); });
  if (!r && r.error().value() == ETIME) {
    return {};
  }
  return r;
}

}  // namespace tinyio
