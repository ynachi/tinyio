#include "tinyio/detail/timer_wheel.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

// The wheel on its own, no runtime: an Arm() is just a Fiber whose
// operation.deadline_ns, timer_armed and timer_index fields the wheel owns.
// These tests target the swap-and-pop index bookkeeping (Arm / Disarm /
// Expire) and the slot arithmetic, which nothing else exercises.

namespace {

using namespace tinyio::detail;

struct Fibers {
  std::vector<fiber::Fiber> fibers;

  explicit Fibers(const size_t n) : fibers(n) {}

  fiber::Fiber* At(const size_t i) { return &fibers[i]; }
};

// Any base works; only (deadline / k_tick_ns) & (k_slots - 1) selects a slot.
constexpr uint64_t k_base = 1ULL << 40;

}  // namespace

TEST(TimerWheelTest, ArmDisarmAreCounted) {
  Fibers fs{3};
  TimerWheel wheel;

  wheel.Arm(fs.At(0), k_base + 5);
  wheel.Arm(fs.At(1), k_base + 7);
  wheel.Arm(fs.At(2), k_base + 9);
  EXPECT_EQ(wheel.HasArmed(), true);

  wheel.Disarm(fs.At(0));
  wheel.Disarm(fs.At(0));  // idempotent
  EXPECT_EQ(wheel.HasArmed(), true);

  wheel.Disarm(fs.At(1));
  wheel.Disarm(fs.At(2));
  EXPECT_EQ(wheel.HasArmed(), false);
}

// The core of the index bookkeeping: removing a middle element moves the
// last fiber into the hole, and the moved fiber's timer_index must follow.
// Disarm the moved one afterwards to prove its index is still valid.
TEST(TimerWheelTest, DisarmMiddleKeepsInvariant) {
  Fibers fs{6};
  TimerWheel wheel;

  for (size_t i = 0; i < 6; ++i) {
    wheel.Arm(fs.At(i), k_base + 1000 * i);
  }

  // Remove a middle element: the fiber armed last swaps into the hole.
  wheel.Disarm(fs.At(2));
  EXPECT_FALSE(fs.At(2)->timer_armed);

  // The moved fiber (originally index 5) must still disarm cleanly.
  wheel.Disarm(fs.At(5));

  // And so must every other one, in a scrambled order.
  wheel.Disarm(fs.At(4));
  wheel.Disarm(fs.At(0));
  wheel.Disarm(fs.At(3));
  wheel.Disarm(fs.At(1));
  EXPECT_EQ(wheel.HasArmed(), false);
}

TEST(TimerWheelTest, ExpireFiresOnlyOverdueDeadlines) {
  Fibers fs{4};
  TimerWheel wheel;

  // All four deadlines share a slot; two have passed at k_base + 2.
  wheel.Arm(fs.At(0), k_base + 1);
  wheel.Arm(fs.At(1), k_base + 2);
  wheel.Arm(fs.At(2), k_base + 3);
  wheel.Arm(fs.At(3), k_base + 4);

  std::vector<fiber::Fiber*> expired;
  wheel.Expire(k_base + 2, [&](fiber::Fiber* f) noexcept { expired.push_back(f); });

  EXPECT_EQ(expired.size(), 2u);
  EXPECT_EQ(expired[0], fs.At(0));
  EXPECT_EQ(expired[1], fs.At(1));
  EXPECT_FALSE(fs.At(0)->timer_armed);
  EXPECT_EQ(fs.At(0)->operation.deadline_ns, 0u);
  EXPECT_TRUE(fs.At(2)->timer_armed);
  EXPECT_TRUE(fs.At(3)->timer_armed);
  EXPECT_EQ(wheel.HasArmed(), true);

  // Disarming a survivor after expiry still works: its index survived the
  // swap-and-pop that removed the expired pair.
  wheel.Disarm(fs.At(2));
  wheel.Disarm(fs.At(3));
  EXPECT_EQ(wheel.HasArmed(), false);
}

// expire() visits every slot it crossed since the last call, so deadlines
// that came due while nobody looked are still caught up.
TEST(TimerWheelTest, ExpireCatchesUpAfterLag) {
  Fibers fs{1};
  TimerWheel wheel;

  wheel.Arm(fs.At(0), k_base + 1);
  std::vector<fiber::Fiber*> expired;
  wheel.Expire(k_base + 17 * TimerWheel::k_tick_ns,
               [&](fiber::Fiber* f) noexcept { expired.push_back(f); });

  EXPECT_EQ(expired.size(), 1u);
  EXPECT_EQ(wheel.HasArmed(), false);
}

// The retry path in IO::ProcessExpiredTimers(): a fiber re-Armed from
// inside the expiry callback, one tick out.
TEST(TimerWheelTest, RearmInsideExpireCallback) {
  Fibers fs{2};
  TimerWheel wheel;

  const uint64_t far = k_base + 3 * TimerWheel::k_tick_ns;
  wheel.Arm(fs.At(0), k_base + 1);
  wheel.Arm(fs.At(1), far);

  std::vector<fiber::Fiber*> expired;
  wheel.Expire(k_base + 2, [&](fiber::Fiber* f) noexcept {
    expired.push_back(f);
    if (f == fs.At(0)) {
      // Same-tick re-arm of an expired fiber: must not fire in this pass.
      wheel.Arm(f, far);
    }
  });

  EXPECT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired[0], fs.At(0));
  EXPECT_TRUE(fs.At(0)->timer_armed);
  EXPECT_TRUE(fs.At(1)->timer_armed);

  // Neither the re-armed deadline nor fs.At(1)'s is due before `far`.
  std::vector<fiber::Fiber*> second_pass;
  wheel.Expire(far - 1, [&](fiber::Fiber* f) noexcept { second_pass.push_back(f); });
  EXPECT_TRUE(second_pass.empty());
  EXPECT_EQ(wheel.HasArmed(), true);
}

// Deadlines past the 32.7 s horizon alias into an earlier slot; expire()
// must check the true deadline and leave them armed.
TEST(TimerWheelTest, HorizonAliasLeavesFutureDeadlineArmed) {
  Fibers fs{1};
  TimerWheel wheel;

  const uint64_t horizon = TimerWheel::k_slots * TimerWheel::k_tick_ns;
  const uint64_t far = k_base + 3 * horizon;
  wheel.Arm(fs.At(0), far);

  // The aliased slot comes due long before the real deadline.
  const uint64_t aliased_due = k_base + (far - k_base) % horizon;
  ASSERT_LT(aliased_due, far);

  std::vector<fiber::Fiber*> expired;
  wheel.Expire(aliased_due + TimerWheel::k_tick_ns,
               [&](fiber::Fiber* f) noexcept { expired.push_back(f); });
  EXPECT_TRUE(expired.empty());
  EXPECT_TRUE(fs.At(0)->timer_armed);
  EXPECT_EQ(wheel.HasArmed(), true);

  // NsUntilNext wakes early for the alias; the next expire finds nothing
  // due, which is the documented trade.
  const auto until = wheel.NsUntilNext(aliased_due);
  ASSERT_TRUE(until.has_value());
  EXPECT_LT(*until, far - aliased_due);
}

TEST(TimerWheelTest, InterleavedArmDisarmExpireStress) {
  Fibers fs{64};
  TimerWheel wheel;

  // Interleave arm / disarm / expire over overlapping deadlines so the
  // swap-and-pop runs with partially filled slots. A broken timer_index
  // trips RemoveFromSlot's assert or leaves a fiber armed forever; both
  // fail the final drain. Fibers are only (re-)armed while disarmed, the
  // same discipline IO::syscall_timed() enforces.
  uint64_t now = k_base;
  for (uint64_t round = 0; round < 32; ++round) {
    for (size_t i = 0; i < fs.fibers.size(); ++i) {
      if (!fs.At(i)->timer_armed) {
        wheel.Arm(fs.At(i),
                  k_base + (round + 1 + (i % 7)) * TimerWheel::k_tick_ns);
      }
      if (i % 3 == 0 && fs.At(i)->timer_armed) {
        wheel.Disarm(fs.At(i));
      }
    }
    now += TimerWheel::k_tick_ns;
    wheel.Expire(now, [](fiber::Fiber*) noexcept {});
  }
  wheel.Expire(now + 8 * TimerWheel::k_tick_ns, [](fiber::Fiber*) noexcept {});

  EXPECT_EQ(wheel.HasArmed(), false);
  for (size_t i = 0; i < fs.fibers.size(); ++i) {
    EXPECT_FALSE(fs.At(i)->timer_armed);
  }
}

TEST(TimerWheelTest, NsUntilNextEmptyIsNullopt) {
  TimerWheel wheel;
  EXPECT_FALSE(wheel.NsUntilNext(k_base).has_value());
}
