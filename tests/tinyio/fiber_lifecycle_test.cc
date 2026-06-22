#include <vector>

#include <gtest/gtest.h>

#include "../../src/tinyio/runtime.hpp"

using namespace tinyio;

namespace
{
class FiberTest : public ::testing::Test
{
protected:
    IoOptions opts;
    IO io{0, opts, {}};
};
}  // namespace

TEST_F(FiberTest, SpawnAndComplete)
{
    bool done = false;

    const auto res = io.spawn([&done](IO&) noexcept { done = true; });

    ASSERT_TRUE(res.has_value()) << "spawning one fiber failed";
    io.run_until_done();

    EXPECT_TRUE(done);
}

TEST_F(FiberTest, SpawnManySequential)
{
    int count = 0;

    for (int i = 0; i < 1000; ++i)
    {
        auto res = io.spawn([&count](IO&) noexcept { count++; });
        ASSERT_TRUE(res.has_value());
        io.run_until_done();
    }

    EXPECT_EQ(count, 1000);
}

TEST_F(FiberTest, YieldRoundRobin)
{
    std::vector<int> exec_order;

    for (int i = 1; i <= 3; ++i)
    {
        auto res = io.spawn(
            [i, &exec_order](IO& rt) noexcept
            {
                for (int j = 0; j < 3; ++j)
                {
                    exec_order.push_back(i);
                    auto yres = rt.yield();
                    if (!yres)
                    {
                        break;
                    }
                }
            });
        ASSERT_TRUE(res.has_value());
    }

    io.run_until_done();

    const std::vector expected = {1, 2, 3, 1, 2, 3, 1, 2, 3};
    EXPECT_EQ(exec_order, expected);
}

TEST_F(FiberTest, NestedSpawn)
{
    bool outer_done = false;
    bool inner_done = false;

    const auto res = io.spawn(
        [&outer_done, &inner_done](IO& rt) noexcept
        {
            const auto inner_res = rt.spawn([&inner_done](IO&) noexcept { inner_done = true; });
            EXPECT_TRUE(inner_res.has_value());
            outer_done = true;
        });

    ASSERT_TRUE(res.has_value());
    io.run_until_done();

    EXPECT_TRUE(outer_done);
    EXPECT_TRUE(inner_done);
}
