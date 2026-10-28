#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "tinyio/runtime/runtime.hpp"

using namespace tinyio;

namespace tinyio {
// In namespace tinyio so that IO's `friend class FiberTest;` names this
// fixture: the test needs the running fiber to check that a large capture
// lives on the fiber's stack, and the runtime keeps that hook private.
class FiberTest : public ::testing::Test {
protected:
    IoOptions opts_;
    IO io_{0, opts_};

    /// True when @p addr points into the stack of the fiber running on the
    /// calling thread.
    ///
    /// The runtime keeps its fiber hook private, and friendship is not
    /// inherited by the class GoogleTest derives from this fixture, so the
    /// check has to live in a member of the fixture itself.
    static bool OnFiberStack(IO& rt, const void* addr) {
        const auto* self = rt.RunningFiber();
        if (self == nullptr) {
            return false;
        }
        const auto value = reinterpret_cast<std::uintptr_t>(addr);
        const auto low = reinterpret_cast<std::uintptr_t>(self->stack.low);
        const auto high = reinterpret_cast<std::uintptr_t>(self->stack.high);
        return value >= low && value < high;
    }
};
}  // namespace tinyio

TEST_F(FiberTest, SpawnAndComplete) {
    bool done = false;

    const auto res = io_.Spawn([&done](IO&) noexcept { done = true; });

    ASSERT_TRUE(res.has_value()) << "spawning one fiber failed";
    io_.RunUntilDone();

    EXPECT_TRUE(done);
}

TEST_F(FiberTest, SpawnManySequential) {
    int count = 0;

    for (int i = 0; i < 1000; ++i) {
        auto res = io_.Spawn([&count](IO&) noexcept { count++; });
        ASSERT_TRUE(res.has_value());
        io_.RunUntilDone();
    }

    EXPECT_EQ(count, 1000);
}

TEST_F(FiberTest, YieldRoundRobin) {
    std::vector<int> exec_order;

    for (int i = 1; i <= 3; ++i) {
        auto res = io_.Spawn([i, &exec_order](IO& rt) noexcept {
            for (int j = 0; j < 3; ++j) {
                exec_order.push_back(i);
                auto yres = rt.Yield();
                if (!yres) {
                    break;
                }
            }
        });
        ASSERT_TRUE(res.has_value());
    }

    io_.RunUntilDone();

    const std::vector expected = {1, 2, 3, 1, 2, 3, 1, 2, 3};
    EXPECT_EQ(exec_order, expected);
}

TEST_F(FiberTest, NestedSpawn) {
    bool outer_done = false;
    bool inner_done = false;

    const auto res = io_.Spawn([&outer_done, &inner_done](IO& rt) noexcept {
        const auto inner_res = rt.Spawn([&inner_done](IO&) noexcept { inner_done = true; });
        EXPECT_TRUE(inner_res.has_value());
        outer_done = true;
    });

    ASSERT_TRUE(res.has_value());
    io_.RunUntilDone();

    EXPECT_TRUE(outer_done);
    EXPECT_TRUE(inner_done);
}

namespace {
struct DropCounter {
    int* drops = nullptr;
    explicit DropCounter(int* d) noexcept : drops(d) {}
    DropCounter(DropCounter&& other) noexcept : drops(other.drops) { other.drops = nullptr; }
    DropCounter& operator=(DropCounter&&) = delete;
    DropCounter(const DropCounter&) = delete;
    DropCounter& operator=(const DropCounter&) = delete;
    ~DropCounter() {
        if (drops != nullptr) {
            ++(*drops);
        }
    }
};
}  // namespace

TEST_F(FiberTest, LargeCaptureLivesOnFiberStackAndDestroysCleanly) {
    std::array<std::uint64_t, 32> payload{};
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = i + 100U;
    }

    int drops = 0;
    std::uint64_t observed_sum = 0;
    bool on_fiber_stack = false;

    auto res = io_.Spawn(
        [payload, tracker = DropCounter(&drops), &observed_sum, &on_fiber_stack](IO& rt) noexcept {
            (void)tracker;
            on_fiber_stack = FiberTest::OnFiberStack(rt, &payload);
            for (const auto v : payload) {
                observed_sum += v;
            }
        });
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(drops, 0);

    io_.RunUntilDone();

    EXPECT_TRUE(on_fiber_stack);
    EXPECT_EQ(observed_sum, 32U * 100U + (31U * 32U) / 2U);
    EXPECT_EQ(drops, 1);
}

TEST(FiberTeardownTest, UnstartedFiberDestroysStackPlacedCallable) {
    int drops = 0;
    {
        IO io{0, IoOptions{}};
        std::array<std::uint64_t, 32> payload{};
        payload.fill(42U);
        auto res = io.Spawn([payload, tracker = DropCounter(&drops)](IO&) noexcept {
            (void)payload;
            (void)tracker;
        });
        ASSERT_TRUE(res.has_value());
        EXPECT_EQ(drops, 0);
    }
    EXPECT_EQ(drops, 1);
}
