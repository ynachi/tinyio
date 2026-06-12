#pragma once

#include "tinyio/intrusive_list.hpp"

#include <array>
#include <cstdint>
#include <ctime>
#include <optional>

#include "fiber.hpp"

namespace tinyio::detail
{

/// Monotonic clock in nanoseconds (immune to wall-clock adjustments).
[[nodiscard]] inline uint64_t monotonic_now_ns() noexcept
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

/// \brief Hashed timing wheel for per-operation I/O timeouts.
///
/// Design goals (vs. io_uring linked timeouts):
///   - Arming / disarming a timeout is two intrusive-list operations:
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
class TimerWheel
{
public:
    static constexpr uint64_t kTickNs = 64ULL * 1000 * 1000;  ///< 64ms slot granularity
    static constexpr size_t kSlots = 512;                     ///< horizon ~32.7s (power of two)
    static_assert((kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");

    using Queue = IntrusiveList<fiber::Fiber, TimerTag, false>;

    /// Arm a timeout for @p f. Pre: f is not currently armed.
    void arm(fiber::Fiber* f, const uint64_t deadline_ns) noexcept
    {
        f->operation.deadline_ns = deadline_ns;
        slots_[slot_of(deadline_ns)].push_back(f);
        ++armed_;
    }

    /// Disarm @p f if armed. O(1), idempotent. Called when the guarded op
    /// completes before its deadline (the overwhelmingly common case).
    void disarm(fiber::Fiber* f) noexcept
    {
        auto* hook = static_cast<IntrusiveHook<TimerTag>*>(f);
        if (!hook->is_linked())
        {
            return;
        }
        slots_[slot_of(f->operation.deadline_ns)].remove(f);
        f->operation.deadline_ns = 0;
        --armed_;
    }

    [[nodiscard]] bool has_armed() const noexcept { return armed_ != 0; }

    /// Pop every fiber whose deadline has passed and hand it to @p on_expired.
    /// Entries whose true deadline is still in the future (same-slot
    /// neighbours, horizon aliases) are left armed.
    template <class Fn>
    void expire(const uint64_t now_ns, Fn&& on_expired) noexcept
    {
        const uint64_t cur = now_ns / kTickNs;
        if (armed_ == 0)
        {
            last_tick_ = cur;
            return;
        }

        // If we lagged more than one full wrap, visiting each slot once covers everything.
        uint64_t from = last_tick_;
        if (cur - from >= kSlots)
        {
            from = cur - (kSlots - 1);
        }

        for (uint64_t t = from; t <= cur; ++t)
        {
            Queue& q = slots_[t & (kSlots - 1)];
            // for_each captures `next` before invoking fn, so removing the
            // current node inside the callback is safe.
            q.for_each(
                [&](fiber::Fiber& f) noexcept
                {
                    if (f.operation.deadline_ns <= now_ns)
                    {
                        q.remove(&f);
                        f.operation.deadline_ns = 0;
                        --armed_;
                        on_expired(&f);
                    }
                });
        }
        last_tick_ = cur;
    }

    /// Nanoseconds until the next candidate deadline, or nullopt when nothing
    /// is armed (== sleep indefinitely). May return early-wake values for
    /// horizon-aliased entries; expire() then finds nothing due, which is fine.
    [[nodiscard]] std::optional<uint64_t> ns_until_next(const uint64_t now_ns) const noexcept
    {
        if (armed_ == 0)
        {
            return std::nullopt;
        }
        const uint64_t cur = now_ns / kTickNs;
        for (uint64_t off = 0; off < kSlots; ++off)
        {
            if (!slots_[(cur + off) & (kSlots - 1)].empty())
            {
                // Wake at the END of the candidate slot so every deadline
                // inside it has passed when we process it.
                const uint64_t target = (cur + off + 1) * kTickNs;
                return target > now_ns ? target - now_ns : 0;
            }
        }
        return std::nullopt;  // unreachable while armed_ > 0
    }

private:
    [[nodiscard]] static size_t slot_of(const uint64_t deadline_ns) noexcept
    {
        return deadline_ns / kTickNs & kSlots - 1;
    }

    std::array<Queue, kSlots> slots_{};
    uint64_t last_tick_ = 0;
    size_t armed_ = 0;
};

}  // namespace tinyio::detail
