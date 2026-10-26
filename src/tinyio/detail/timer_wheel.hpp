#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <optional>
#include <vector>

#include "tinyio/detail/fiber.hpp"

namespace tinyio::detail {

/// Monotonic clock in nanoseconds (immune to wall-clock adjustments).
[[nodiscard]] inline uint64_t MonotonicNowNs() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
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
    static constexpr size_t k_slots = 512;                      ///< horizon ~32.7s (power of two)
    static constexpr size_t k_words = k_slots / 64;             ///< 512-bit occupancy bitmap
    static_assert((k_slots & (k_slots - 1)) == 0, "kSlots must be a power of two");
    static_assert(k_slots % 64 == 0, "kSlots must be a multiple of 64");

    /// The fibers armed in one slot, in no particular order. Each fiber
    /// remembers its position (Fiber::timer_index), so removal is a
    /// swap-and-pop instead of a search.
    using Slot = std::vector<fiber::Fiber*>;

    /// Arm a timeout for @p f. Pre: f is not currently armed.
    void Arm(fiber::Fiber* f, const uint64_t deadline_ns) noexcept {
        const size_t idx = SlotOf(deadline_ns);
        Slot& slot = slots_[idx];
        f->operation.deadline_ns = deadline_ns;
        f->timer_armed = true;
        f->timer_index = slot.size();
        slot.push_back(f);
        occupied_[idx >> 6U] |= (uint64_t{1} << (idx & 63U));
        ++armed_;
    }

    /// Disarm @p f if armed. O(1), idempotent. Called when the guarded op
    /// completes before its deadline (the overwhelmingly common case).
    void Disarm(fiber::Fiber* f) noexcept {
        if (!f->timer_armed) {
            return;
        }
        RemoveFromSlot(SlotOf(f->operation.deadline_ns), f);
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
            const size_t idx = static_cast<size_t>(t & (k_slots - 1));
            if ((occupied_[idx >> 6U] & (uint64_t{1} << (idx & 63U))) == 0) {
                continue;
            }
            Slot& slot = slots_[idx];
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
                RemoveFromSlot(idx, f);
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
    [[nodiscard]] std::optional<uint64_t> NsUntilNext(const uint64_t now_ns) const noexcept {
        if (armed_ == 0) {
            return std::nullopt;
        }

        const uint64_t cur = now_ns / k_tick_ns;
        const size_t start_slot = static_cast<size_t>(cur & (k_slots - 1));
        const size_t start_word = start_slot >> 6U;
        const size_t start_bit = start_slot & 63U;

        auto wake_at_offset = [&](const uint64_t off) noexcept -> std::optional<uint64_t> {
            // Wake at the END of the candidate slot so every deadline
            // inside it has passed when we process it.
            const uint64_t target = (cur + off + 1) * k_tick_ns;
            return target > now_ns ? target - now_ns : 0;
        };

        // 1. Remainder of the starting 64-slot word [start_bit, 63].
        if (const uint64_t mask = occupied_[start_word] >> start_bit; mask != 0) {
            return wake_at_offset(CountrZero(mask));
        }

        // 2. The intermediate 7 words.
        for (size_t w = 1; w < k_words; ++w) {
            if (const uint64_t word = occupied_[(start_word + w) & (k_words - 1)]; word != 0) {
                const uint64_t off =
                    (static_cast<uint64_t>(w) * uint64_t{64} - static_cast<uint64_t>(start_bit)) +
                    CountrZero(word);
                return wake_at_offset(off);
            }
        }

        // 3. Wrapped low bits [0, start_bit) of the starting word.
        if (const uint64_t word = occupied_[start_word]; word != 0) {
            const uint64_t off =
                (static_cast<uint64_t>(k_slots) - static_cast<uint64_t>(start_bit)) +
                CountrZero(word);
            return wake_at_offset(off);
        }

        // unreachable while armed_ > 0
        return std::nullopt;
    }

private:
    [[nodiscard]] static constexpr uint64_t CountrZero(const uint64_t word) noexcept {
        return static_cast<uint64_t>(static_cast<unsigned int>(std::countr_zero(word)));
    }

    [[nodiscard]] static size_t SlotOf(const uint64_t deadline_ns) noexcept {
        return static_cast<size_t>((deadline_ns / k_tick_ns) & (k_slots - 1));
    }

    // Swap-and-pop: move the slot's last fiber into f's position.
    //
    // Precondition (the invariant every caller depends on): while
    // f->timer_armed, slot[f->timer_index] == f, in
    // slots_[SlotOf(f->operation.deadline_ns)]. Arm() and this function
    // maintain it; if it ever breaks, the write below lands in the wrong
    // vector position and silently corrupts another fiber's entry.
    void RemoveFromSlot(const size_t idx, fiber::Fiber* f) noexcept {
        Slot& slot = slots_[idx];
        assert(slot[f->timer_index] == f && "timer_index invariant violated");
        fiber::Fiber* last = slot.back();
        slot[f->timer_index] = last;
        last->timer_index = f->timer_index;
        slot.pop_back();
        f->timer_armed = false;
        if (slot.empty()) {
            occupied_[idx >> 6U] &= ~(uint64_t{1} << (idx & 63U));
        }
    }

    std::array<uint64_t, k_words> occupied_{};
    std::array<Slot, k_slots> slots_{};
    uint64_t last_tick_ = 0;
    size_t armed_ = 0;
};

}  // namespace tinyio::detail
