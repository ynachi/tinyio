#pragma once

#include <array>
#include <cassert>
#include <optional>
#include <vector>

#include "fiber.hpp"

namespace tinyio::detail {

/// Monotonic clock in nanoseconds (immune to wall-clock adjustments).
[[nodiscard]] inline uint64_t MonotonicNowNs() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

/// \brief Hashed timing wheel for per-operation I/O timeouts.
///
/// Design goals (vs. io_uring linked timeouts):
///   - Arming / disarming a timeout is a vector push / swap-and-pop:
///     no SQE, no kernel hrtimer, no extra CQE on the hot path.
///   - The kernel is only involved when a timeout actually FIRES
///     (rare path: one ASYNC_CANCEL per expired op).
///   - The scheduler sleeps with a bounded wait computed from the
///     nearest armed deadline (io_uring_submit_and_wait_timeout /
///     IORING_ENTER_EXT_ARG — a syscall argument, not an SQE).
///
/// Granularity: deadlines are honored with up to kTickNs (64ms) of slack,
/// which is the right trade for I/O timeouts measured in seconds.
///
/// Deadlines beyond the wheel horizon (kSlots * kTickNs ~ 32.7s) alias into
/// their slot modulo the horizon. expire() checks the true deadline before
/// firing, so aliased entries simply cause one early (no-op) wakeup per
/// horizon — bounded and harmless.
///
/// Single-threaded: owner-thread only, like the rest of the runtime.
class TimerWheel {
 public:
  static constexpr uint64_t k_tick_ns = 64ULL * 1000 * 1000;  ///< 64ms slot granularity
  static constexpr size_t k_slots = 512;  ///< horizon ~32.7s (power of two)
  static_assert((k_slots & (k_slots - 1)) == 0, "kSlots must be a power of two");

  /// The fibers armed in one slot, in no particular order. Each fiber
  /// remembers its position (Fiber::timer_index), so removal is a
  /// swap-and-pop instead of a search.
  using Slot = std::vector<fiber::Fiber*>;

  /// Arm a timeout for @p f. Pre: f is not currently armed.
  void Arm(fiber::Fiber* f, const uint64_t deadline_ns) noexcept {
    Slot& slot = slots_[SlotOf(deadline_ns)];
    f->operation.deadline_ns = deadline_ns;
    f->timer_armed = true;
    f->timer_index = slot.size();
    slot.push_back(f);
    ++armed_;
  }

  /// Disarm @p f if armed. O(1), idempotent. Called when the guarded op
  /// completes before its deadline (the overwhelmingly common case).
  void Disarm(fiber::Fiber* f) noexcept {
    if (!f->timer_armed) {
      return;
    }
    RemoveFromSlot(slots_[SlotOf(f->operation.deadline_ns)], f);
    f->operation.deadline_ns = 0;
    --armed_;
  }

  [[nodiscard]] bool HasArmed() const noexcept { return armed_ != 0; }

  /// Pop every fiber whose deadline has passed and hand it to on_expired.
  /// Entries whose true deadline is still in the future (same-slot
  /// neighbours, horizon aliases) are left armed.
  template <class Fn>
  void Expire(const uint64_t now_ns, Fn&& on_expired) noexcept {
    const uint64_t cur = now_ns / k_tick_ns;
    if (armed_ == 0) {
      last_tick_ = cur;
      return;
    }

    // If we lagged more than one full wrap, visiting each slot once covers everything.
    uint64_t from = last_tick_;
    if (cur - from >= k_slots) {
      from = cur - (k_slots - 1);
    }

    for (uint64_t t = from; t <= cur; ++t) {
      Slot& slot = slots_[t & (k_slots - 1)];
      // Index loop, re-reading the size every time: removing slot[i] moves
      // the last fiber into position i, so i must not advance after a
      // removal. on_expired() may also Arm() into this very slot (push_back
      // can reallocate), which is why nothing holds a reference to an
      // element across the callback.
      std::size_t i = 0;
      while (i < slot.size()) {
        fiber::Fiber* f = slot[i];
        if (f->operation.deadline_ns > now_ns) {
          ++i;
          continue;
        }
        RemoveFromSlot(slot, f);
        f->operation.deadline_ns = 0;
        --armed_;
        on_expired(f);
      }
    }
    last_tick_ = cur;
  }

  /// Nanoseconds until the next candidate deadline, or nullopt when nothing
  /// is armed (== sleep indefinitely). May return early-wake values for
  /// horizon-aliased entries; expire() then finds nothing due, which is fine.
  [[nodiscard]] std::optional<uint64_t> NsUntilNext(
      const uint64_t now_ns) const noexcept {
    if (armed_ == 0) {
      return std::nullopt;
    }

    const uint64_t cur = now_ns / k_tick_ns;
    for (uint64_t off = 0; off < k_slots; ++off) {
      if (!slots_[(cur + off) & (k_slots - 1)].empty()) {
        // Wake at the END of the candidate slot so every deadline
        // inside it has passed when we process it.
        const uint64_t target = (cur + off + 1) * k_tick_ns;
        return target > now_ns ? target - now_ns : 0;
      }
    }

    // unreachable while armed_ > 0
    return std::nullopt;
  }

 private:
  [[nodiscard]] static size_t SlotOf(const uint64_t deadline_ns) noexcept {
    return (deadline_ns / k_tick_ns) & (k_slots - 1);
  }

  // Swap-and-pop: move the slot's last fiber into f's position.
  //
  // Precondition (the invariant every caller depends on): while
  // f->timer_armed, slot[f->timer_index] == f, in
  // slots_[SlotOf(f->operation.deadline_ns)]. Arm() and this function
  // maintain it; if it ever breaks, the write below lands in the wrong
  // vector position and silently corrupts another fiber's entry.
  static void RemoveFromSlot(Slot& slot, fiber::Fiber* f) noexcept {
    assert(slot[f->timer_index] == f && "timer_index invariant violated");
    fiber::Fiber* last = slot.back();
    slot[f->timer_index] = last;
    last->timer_index = f->timer_index;
    slot.pop_back();
    f->timer_armed = false;
  }

  std::array<Slot, k_slots> slots_{};
  uint64_t last_tick_ = 0;
  size_t armed_ = 0;
};

}  // namespace tinyio::detail
