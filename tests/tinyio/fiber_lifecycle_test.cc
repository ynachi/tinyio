#include "tinyio/runtime.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <vector>

using namespace tinyio;

// Helper to access private members of IO for testing
template <typename Tag, typename Tag::type M>
struct Accessor {
    friend typename Tag::type get(Tag) { return M; }
};

struct IdleFibersTag {
    using type = IntrusiveList<detail::fiber::Fiber, ReadyQueueTag> IO::*;
};
template struct Accessor<IdleFibersTag, &IO::idle_fibers_>;
IntrusiveList<detail::fiber::Fiber, ReadyQueueTag> IO::* get(IdleFibersTag);

struct AllFibersTagAccess {
    using type = IntrusiveList<detail::fiber::Fiber, AllFibersTag> IO::*;
};
template struct Accessor<AllFibersTagAccess, &IO::all_fibers_>;
IntrusiveList<detail::fiber::Fiber, AllFibersTag> IO::* get(AllFibersTagAccess);

// Group A — Fiber lifecycle tests
// These tests verify that the core fiber state machine works correctly, and that 
// fibers are created, recycled, and destroyed without leaks or corruption.

// 1. spawn_and_complete
TEST(FiberLifecycleTest, SpawnAndComplete)
{
    IoOptions opts;
    IO io(0, opts, {});
    bool done = false;
    
    auto res = io.spawn([&done](IO&) noexcept {
        done = true;
    });
    
    ASSERT_TRUE(res.has_value());
    io.run_until_done();
    
    EXPECT_TRUE(done);
}

// 2. spawn_many_sequential
TEST(FiberLifecycleTest, SpawnManySequential)
{
    IoOptions opts;
    IO io(0, opts, {});
    int count = 0;
    
    for (int i = 0; i < 1000; ++i) {
        auto res = io.spawn([&count](IO&) noexcept {
            count++;
        });
        ASSERT_TRUE(res.has_value());
        io.run_until_done();
    }
    
    EXPECT_EQ(count, 1000);
}

// 3. spawn_many_concurrent
TEST(FiberLifecycleTest, SpawnManyConcurrent)
{
    IoOptions opts;
    IO io(0, opts, {});
    std::atomic<int> count{0};
    
    for (int i = 0; i < 500; ++i) {
        auto res = io.spawn([&count](IO&) noexcept {
            count.fetch_add(1, std::memory_order_relaxed);
        });
        ASSERT_TRUE(res.has_value());
    }
    
    io.run_until_done();
    EXPECT_EQ(count.load(), 500);
}

// 4. yield_round_robin
TEST(FiberLifecycleTest, YieldRoundRobin)
{
    IoOptions opts;
    IO io(0, opts, {});
    std::vector<int> exec_order;
    
    for (int i = 1; i <= 3; ++i) {
        auto res = io.spawn([i, &exec_order](IO& rt) noexcept {
            for (int j = 0; j < 3; ++j) {
                exec_order.push_back(i);
                auto yres = rt.yield();
                if (!yres) {
                    break;
                }
            }
        });
        ASSERT_TRUE(res.has_value());
    }
    
    io.run_until_done();
    
    std::vector<int> expected = {1, 2, 3, 1, 2, 3, 1, 2, 3};
    EXPECT_EQ(exec_order, expected);
}

// 5. fiber_pool_hwm
TEST(FiberLifecycleTest, FiberPoolHwm)
{
    IoOptions opts;
    opts.max_idle_fibers = 4;
    IO io(0, opts, {});
    
    std::atomic<int> done_count{0};
    for (int i = 0; i < 10; ++i) {
        auto res = io.spawn([&done_count](IO&) noexcept {
            done_count++;
        });
        ASSERT_TRUE(res.has_value());
    }
    
    io.run_until_done();
    
    EXPECT_EQ(done_count.load(), 10);
    
    auto idle_fibers_ptr = get(IdleFibersTag{});
    auto all_fibers_ptr = get(AllFibersTagAccess{});
    
    EXPECT_LE((io.*idle_fibers_ptr).size(), 4);
    EXPECT_EQ((io.*all_fibers_ptr).size(), (io.*idle_fibers_ptr).size());
}

// 6. nested_spawn
TEST(FiberLifecycleTest, NestedSpawn)
{
    IoOptions opts;
    IO io(0, opts, {});
    
    bool outer_done = false;
    bool inner_done = false;
    
    auto res = io.spawn([&outer_done, &inner_done](IO& rt) noexcept {
        auto inner_res = rt.spawn([&inner_done](IO&) noexcept {
            inner_done = true;
        });
        EXPECT_TRUE(inner_res.has_value());
        outer_done = true;
    });
    
    ASSERT_TRUE(res.has_value());
    io.run_until_done();
    
    EXPECT_TRUE(outer_done);
    EXPECT_TRUE(inner_done);
}
