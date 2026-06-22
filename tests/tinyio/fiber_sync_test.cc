#include "../../src/tinyio/fiber_sync.hpp"

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "../../src/tinyio/error.hpp"

using namespace tinyio;

// Common fixture: a fresh single-threaded IO context for each test.
class FiberSyncTest : public ::testing::Test
{
protected:
    IoOptions opts;
    IO io{0, opts, {}};
};

// ── FiberLatch ──────────────────────────────────────────────────────────────

// N workers count down; one waiter unblocks only after all N have fired.
TEST_F(FiberSyncTest, LatchBasic)
{
    int count = 0;

    (void)io.spawn([&count](IO& rt) noexcept {
        Latch latch(rt, 3);

        for (int i = 0; i < 3; ++i) {
            (void)rt.spawn([&latch, &count](IO&) noexcept {
                count++;
                latch.count_down();
            });
        }

        (void)latch.wait();
        EXPECT_EQ(count, 3);
    });

    io.run_until_done();
    EXPECT_EQ(count, 3);
}

// wait() on an already-zero latch must return without suspending.
TEST_F(FiberSyncTest, LatchWaitReturnsImmediatelyWhenCountIsZero)
{
    bool reached_after_wait = false;

    (void)io.spawn([&](IO& rt) noexcept {
        Latch latch(rt, 0);
        (void)latch.wait();
        reached_after_wait = true;
    });

    io.run_until_done();
    EXPECT_TRUE(reached_after_wait);
}

// count_down(n) decrements by n in one call, crossing zero atomically.
TEST_F(FiberSyncTest, LatchCountDownByN)
{
    bool reached = false;

    (void)io.spawn([&](IO& rt) noexcept {
        Latch latch(rt, 3);

        (void)rt.spawn([&](IO&) noexcept {
            latch.count_down(3);
        });

        (void)latch.wait();
        reached = true;
    });

    io.run_until_done();
    EXPECT_TRUE(reached);
}

// All fibers blocked on wait() are woken simultaneously when count hits 0.
TEST_F(FiberSyncTest, LatchMultipleWaiters)
{
    int waiters_done = 0;

    (void)io.spawn([&](IO& rt) noexcept {
        Latch gate(rt, 1);
        Latch done(rt, 3);

        for (int i = 0; i < 3; ++i) {
            (void)rt.spawn([&](IO&) noexcept {
                (void)gate.wait();
                waiters_done++;
                done.count_down();
            });
        }

        // Yield once per spawned child so each reaches gate.wait() and suspends
        // before we fire the gate.
        (void)rt.yield();
        (void)rt.yield();
        (void)rt.yield();

        gate.count_down();  // wakes all 3 simultaneously
        (void)done.wait();
        EXPECT_EQ(waiters_done, 3);
    });

    io.run_until_done();
    EXPECT_EQ(waiters_done, 3);
}

// ── FiberSemaphore ──────────────────────────────────────────────────────────

// Semaphore limits concurrent active fibers to its initial count.
TEST_F(FiberSyncTest, SemaphoreBasic)
{
    int active = 0;
    int max_active = 0;

    (void)io.spawn([&](IO& rt) noexcept {
        Semaphore sem(rt, 2);
        Latch done(rt, 5);

        for (int i = 0; i < 5; ++i) {
            (void)rt.spawn([&](IO& inner) noexcept {
                (void)sem.acquire();

                active++;
                if (active > max_active)
                    max_active = active;

                (void)inner.yield();

                active--;
                sem.release();
                done.count_down();
            });
        }

        (void)done.wait();
        EXPECT_LE(max_active, 2);
    });

    io.run_until_done();
}

// try_acquire succeeds (and decrements count) when slots are available.
TEST_F(FiberSyncTest, SemaphoreTryAcquireSucceedsWhenAvailable)
{
    bool acquired = false;

    (void)io.spawn([&](IO& rt) noexcept {
        Semaphore sem(rt, 1);
        acquired = sem.try_acquire();
    });

    io.run_until_done();
    EXPECT_TRUE(acquired);
}

// try_acquire returns false instead of blocking when count is 0.
TEST_F(FiberSyncTest, SemaphoreTryAcquireFailsWhenExhausted)
{
    bool second = false;

    (void)io.spawn([&](IO& rt) noexcept {
        Semaphore sem(rt, 1);
        EXPECT_TRUE(sem.try_acquire());  // drains the slot
        second = sem.try_acquire();      // nothing left
    });

    io.run_until_done();
    EXPECT_FALSE(second);
}

// Waiters are woken in FIFO order (the wait queue is a fair queue).
TEST_F(FiberSyncTest, SemaphoreWaitersWokenFifo)
{
    std::vector<int> order;

    (void)io.spawn([&](IO& rt) noexcept {
        Semaphore sem(rt, 0);  // start exhausted so every acquire blocks
        Latch done(rt, 3);

        for (int i = 1; i <= 3; ++i) {
            (void)rt.spawn([&, i](IO&) noexcept {
                (void)sem.acquire();
                order.push_back(i);
                done.count_down();
            });
        }

        // Let all 3 reach acquire() and enqueue as waiters before releasing.
        (void)rt.yield();
        (void)rt.yield();
        (void)rt.yield();

        sem.release();
        sem.release();
        sem.release();
        (void)done.wait();
    });

    io.run_until_done();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

// ── FiberMutex ──────────────────────────────────────────────────────────────

// Mutex serialises access: each fiber sees a consistent shared value.
TEST_F(FiberSyncTest, MutexBasic)
{
    int shared = 0;

    (void)io.spawn([&](IO& rt) noexcept {
        Mutex m(rt);
        Latch done(rt, 5);

        for (int i = 0; i < 5; ++i) {
            (void)rt.spawn([&](IO& inner) noexcept {
                auto guard = m.scoped_lock().value();
                int local = shared;
                (void)inner.yield();  // try to induce a race
                shared = local + 1;
                done.count_down();
            });
        }

        (void)done.wait();
        EXPECT_EQ(shared, 5);
    });

    io.run_until_done();
}

// try_lock succeeds and sets the held flag when the mutex is free.
TEST_F(FiberSyncTest, MutexTryLockSucceedsWhenFree)
{
    bool locked = false;

    (void)io.spawn([&](IO& rt) noexcept {
        Mutex m(rt);
        locked = m.try_lock();
        if (locked)
            m.unlock();
    });

    io.run_until_done();
    EXPECT_TRUE(locked);
}

// try_lock returns false without blocking when the mutex is already held.
TEST_F(FiberSyncTest, MutexTryLockFailsWhenHeld)
{
    bool second = false;

    (void)io.spawn([&](IO& rt) noexcept {
        Mutex m(rt);
        EXPECT_TRUE(m.try_lock());
        second = m.try_lock();  // already held
        m.unlock();
    });

    io.run_until_done();
    EXPECT_FALSE(second);
}

// The Guard returned by scoped_lock() unlocks the mutex when it goes out of scope.
TEST_F(FiberSyncTest, MutexScopedLockReleasesOnDestruct)
{
    bool relocked = false;

    (void)io.spawn([&](IO& rt) noexcept {
        Mutex m(rt);
        {
            auto g = m.scoped_lock().value();
            EXPECT_FALSE(m.try_lock());  // held by g
        }  // g destructs → unlock()
        relocked = m.try_lock();         // now free again
        if (relocked)
            m.unlock();
    });

    io.run_until_done();
    EXPECT_TRUE(relocked);
}

// Ownership is transferred to waiters in FIFO order.
TEST_F(FiberSyncTest, MutexWaitersWokenFifo)
{
    std::vector<int> order;

    (void)io.spawn([&](IO& rt) noexcept {
        Mutex m(rt);
        Latch done(rt, 3);

        (void)m.lock();  // outer fiber holds the lock

        for (int i = 1; i <= 3; ++i) {
            (void)rt.spawn([&, i](IO&) noexcept {
                (void)m.lock();  // blocks — mutex is held by outer
                order.push_back(i);
                m.unlock();
                done.count_down();
            });
        }

        // Let all 3 fibers enqueue as waiters before we release.
        (void)rt.yield();
        (void)rt.yield();
        (void)rt.yield();

        m.unlock();  // transfers ownership to fiber 1
        (void)done.wait();
    });

    io.run_until_done();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

// ── ResultGroup ─────────────────────────────────────────────────────────────

// All tasks run and their return values are accessible via results() after wait().
TEST_F(FiberSyncTest, ResultGroupBasic)
{
    (void)io.spawn([](IO& rt) noexcept {
        ResultGroup<int> group(rt, 3);

        group.spawn([](IO&) noexcept -> Result<int> { return 10; });
        group.spawn([](IO&) noexcept -> Result<int> { return 20; });
        group.spawn([](IO&) noexcept -> Result<int> { return 30; });

        (void)group.wait();

        auto& r = group.results();
        ASSERT_TRUE(r[0].has_value() && r[0]->has_value()); EXPECT_EQ(r[0]->value(), 10);
        ASSERT_TRUE(r[1].has_value() && r[1]->has_value()); EXPECT_EQ(r[1]->value(), 20);
        ASSERT_TRUE(r[2].has_value() && r[2]->has_value()); EXPECT_EQ(r[2]->value(), 30);
    });

    io.run_until_done();
}

// A task that returns an error populates its slot with that error.
TEST_F(FiberSyncTest, ResultGroupTaskError)
{
    (void)io.spawn([](IO& rt) noexcept {
        ResultGroup<int> group(rt, 2);

        group.spawn([](IO&) noexcept -> Result<int> { return 42; });
        group.spawn([](IO&) noexcept -> Result<int> {
            return std::unexpected(Error{EINVAL, "test-op"});
        });

        (void)group.wait();

        auto& r = group.results();
        ASSERT_TRUE(r[0].has_value() && r[0]->has_value());
        EXPECT_EQ(r[0]->value(), 42);

        ASSERT_TRUE(r[1].has_value() && !r[1]->has_value());
        EXPECT_EQ(r[1]->error().code, EINVAL);
    });

    io.run_until_done();
}

// Slots that were never spawned remain std::nullopt.
TEST_F(FiberSyncTest, ResultGroupUnspawnedSlotIsNullopt)
{
    (void)io.spawn([](IO& rt) noexcept {
        ResultGroup<int> group(rt, 3);

        group.spawn([](IO&) noexcept -> Result<int> { return 1; });
        // slots 1 and 2 are never spawned

        (void)group.wait();

        auto& r = group.results();
        EXPECT_TRUE(r[0].has_value());
        EXPECT_FALSE(r[1].has_value());  // nullopt
        EXPECT_FALSE(r[2].has_value());  // nullopt
    });

    io.run_until_done();
}

// ── TaskGroup ───────────────────────────────────────────────────────────────

// wait() unblocks only after all spawned children have completed.
TEST_F(FiberSyncTest, TaskGroupBasic)
{
    int count = 0;

    (void)io.spawn([&count](IO& rt) noexcept {
        TaskGroup group(rt);

        ASSERT_TRUE(group.spawn([&count](IO&) noexcept { count += 1; }).has_value());
        ASSERT_TRUE(group.spawn([&count](IO&) noexcept { count += 2; }).has_value());

        (void)group.wait();
        EXPECT_EQ(count, 3);
    });

    io.run_until_done();
    EXPECT_EQ(count, 3);
}

// 3 sleeps done one-after-another (sequential) vs. 3 sleeps issued simultaneously
// via TaskGroup (concurrent). The concurrent case should finish in ~1× the sleep
// duration while the sequential case takes ~3×, demonstrating that TaskGroup
// overlaps I/O wait time across fibers.
TEST_F(FiberSyncTest, TaskGroupConcurrentSleepsFasterThanSequential)
{
    using namespace std::chrono;
    constexpr nanoseconds sleep_dur = milliseconds(20);

    nanoseconds sequential_elapsed{};
    nanoseconds concurrent_elapsed{};

    // Sequential: each sleep must complete before the next one starts.
    (void)io.spawn([&, sleep_dur](IO& rt) noexcept {
        auto t0 = steady_clock::now();
        for (int i = 0; i < 3; ++i)
            (void)rt.sleep_for_ns(sleep_dur);
        sequential_elapsed = steady_clock::now() - t0;
    });
    io.run_until_done();

    // Concurrent: all 3 sleeps are in flight simultaneously.
    (void)io.spawn([&, sleep_dur](IO& rt) noexcept {
        TaskGroup group(rt);
            const auto t0 = steady_clock::now();
        for (int i = 0; i < 3; ++i)
            (void)group.spawn([sleep_dur](IO& inner) noexcept {
                (void)inner.sleep_for_ns(sleep_dur);
            });
        (void)group.wait();
        concurrent_elapsed = steady_clock::now() - t0;
    });
    io.run_until_done();

    // Sequential ≈ 3 × sleep_dur, concurrent ≈ 1 × sleep_dur.
    // Require concurrent < sequential / 2 to tolerate scheduler jitter.
    EXPECT_LT(concurrent_elapsed, sequential_elapsed / 2)
        << "sequential=" << duration_cast<milliseconds>(sequential_elapsed).count() << "ms"
        << "  concurrent=" << duration_cast<milliseconds>(concurrent_elapsed).count() << "ms";
}

// wait() with no tasks spawned must return immediately without suspending.
TEST_F(FiberSyncTest, TaskGroupWaitWithNoTasksReturnsImmediately)
{
    bool reached = false;

    (void)io.spawn([&](IO& rt) noexcept {
        TaskGroup group(rt);
        (void)group.wait();
        reached = true;
    });

    io.run_until_done();
    EXPECT_TRUE(reached);
}
