#include "tinyio/fiber_sync.hpp"
#include <gtest/gtest.h>
#include <vector>

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
        FiberLatch latch(rt, 3);

        for (int i = 0; i < 3; ++i) {
            (void)rt.spawn([&latch, &count](IO&) noexcept {
                count++;
                latch.count_down();
            });
        }

        latch.wait();
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
        FiberLatch latch(rt, 0);
        latch.wait();
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
        FiberLatch latch(rt, 3);

        (void)rt.spawn([&](IO&) noexcept {
            latch.count_down(3);
        });

        latch.wait();
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
        FiberLatch gate(rt, 1);
        FiberLatch done(rt, 3);

        for (int i = 0; i < 3; ++i) {
            (void)rt.spawn([&](IO&) noexcept {
                gate.wait();
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
        done.wait();
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
        FiberSemaphore sem(rt, 2);
        FiberLatch done(rt, 5);

        for (int i = 0; i < 5; ++i) {
            (void)rt.spawn([&](IO& inner) noexcept {
                sem.acquire();

                active++;
                if (active > max_active)
                    max_active = active;

                (void)inner.yield();

                active--;
                sem.release();
                done.count_down();
            });
        }

        done.wait();
        EXPECT_LE(max_active, 2);
    });

    io.run_until_done();
}

// try_acquire succeeds (and decrements count) when slots are available.
TEST_F(FiberSyncTest, SemaphoreTryAcquireSucceedsWhenAvailable)
{
    bool acquired = false;

    (void)io.spawn([&](IO& rt) noexcept {
        FiberSemaphore sem(rt, 1);
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
        FiberSemaphore sem(rt, 1);
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
        FiberSemaphore sem(rt, 0);  // start exhausted so every acquire blocks
        FiberLatch done(rt, 3);

        for (int i = 1; i <= 3; ++i) {
            (void)rt.spawn([&, i](IO&) noexcept {
                sem.acquire();
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
        done.wait();
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
        FiberMutex m(rt);
        FiberLatch done(rt, 5);

        for (int i = 0; i < 5; ++i) {
            (void)rt.spawn([&](IO& inner) noexcept {
                auto guard = m.scoped_lock();
                int local = shared;
                (void)inner.yield();  // try to induce a race
                shared = local + 1;
                done.count_down();
            });
        }

        done.wait();
        EXPECT_EQ(shared, 5);
    });

    io.run_until_done();
}

// try_lock succeeds and sets the held flag when the mutex is free.
TEST_F(FiberSyncTest, MutexTryLockSucceedsWhenFree)
{
    bool locked = false;

    (void)io.spawn([&](IO& rt) noexcept {
        FiberMutex m(rt);
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
        FiberMutex m(rt);
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
        FiberMutex m(rt);
        {
            auto g = m.scoped_lock();
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
        FiberMutex m(rt);
        FiberLatch done(rt, 3);

        m.lock();  // outer fiber holds the lock

        for (int i = 1; i <= 3; ++i) {
            (void)rt.spawn([&, i](IO&) noexcept {
                m.lock();  // blocks — mutex is held by outer
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
        done.wait();
    });

    io.run_until_done();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

// ── TaskGroup ───────────────────────────────────────────────────────────────

// wait() unblocks only after all spawned children have completed.
TEST_F(FiberSyncTest, TaskGroupBasic)
{
    int count = 0;

    (void)io.spawn([&count](IO& rt) noexcept {
        TaskGroup group(rt);

        group.spawn([&count](IO&) noexcept { count += 1; });
        group.spawn([&count](IO&) noexcept { count += 2; });

        group.wait();
        EXPECT_EQ(count, 3);
    });

    io.run_until_done();
    EXPECT_EQ(count, 3);
}

// wait() with no tasks spawned must return immediately without suspending.
TEST_F(FiberSyncTest, TaskGroupWaitWithNoTasksReturnsImmediately)
{
    bool reached = false;

    (void)io.spawn([&](IO& rt) noexcept {
        TaskGroup group(rt);
        group.wait();
        reached = true;
    });

    io.run_until_done();
    EXPECT_TRUE(reached);
}
