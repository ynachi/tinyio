#include "tinyio/runtime/runtime.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#include <ylt/easylog.hpp>

namespace tinyio {
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

}  // namespace tinyio
