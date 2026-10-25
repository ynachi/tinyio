#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <gtest/gtest.h>

#include "tinyio/runtime/fiber_sync.hpp"
#include "tinyio/runtime/runtime.hpp"

using namespace tinyio;

// Helper to access private members of IO for testing
template <typename Tag, typename Tag::type M>
struct Accessor {
    friend Tag::type get(Tag) { return M; }
};

// Group B — Shutdown and cancellation tests

// 1. stop_token_cancels_waiting
TEST(ShutdownCancellationTest, StopTokenCancelsWaiting) {
    IoOptions opts;
    IO io(0, opts);
    std::stop_source ss;

    bool fiber_ran = false;
    Error fiber_err;

    auto res = io.Spawn([&](IO& rt) noexcept {
        fiber_ran = true;
        auto sleep_res = rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));  // 10s
        if (!sleep_res) {
            fiber_err = sleep_res.error();
        }
    });
    ASSERT_TRUE(res.has_value());

    auto start = std::chrono::steady_clock::now();

    std::thread t([&ss]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ss.request_stop();
    });

    io.Run(ss.get_token());
    t.join();

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_TRUE(fiber_ran);
    EXPECT_EQ(fiber_err.Value(), ECANCELED);
    EXPECT_LT(elapsed.count(), 2000);
}

// 2. stop_token_cancels_ready
TEST(ShutdownCancellationTest, StopTokenCancelsReady) {
    IoOptions opts;
    IO io(0, opts);
    std::stop_source ss;

    bool fiber_started = false;
    bool fiber_finished = false;
    tinyio::Error yield_err;

    auto res = io.Spawn([&](IO& rt) noexcept {
        fiber_started = true;
        while (true) {
            auto yield_res = rt.Yield();
            if (!yield_res) {
                yield_err = yield_res.error();
                break;
            }
        }
        fiber_finished = true;
    });
    ASSERT_TRUE(res.has_value());

    std::thread t([&ss]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ss.request_stop();
    });

    io.Run(ss.get_token());
    t.join();

    EXPECT_TRUE(fiber_started);
    EXPECT_TRUE(fiber_finished);
    EXPECT_EQ(yield_err.Value(), ECANCELED);
}

// 3. destructor_cancels_live_work
TEST(ShutdownCancellationTest, DestructorCancelsLiveWork) {
    IoOptions opts;
    {
        IO io(0, opts);
        auto res = io.Spawn([](IO& rt) noexcept {
            (void)rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));  // 10s
        });
        ASSERT_TRUE(res.has_value());
    }
}

// Accessor for activate()
struct ActivateTag {
    using type = Result<void> (IO::*)() noexcept;
};
template struct Accessor<ActivateTag, &IO::Activate>;
Result<void> (IO::* get(ActivateTag))() noexcept;

// 4. destructor_aborts_wrong_thread
TEST(ShutdownCancellationTest, DestructorAbortsWrongThread) {
    IoOptions opts;
    EXPECT_DEATH(
        {
            auto io = std::make_unique<IO>(0, opts);

            auto activate_ptr = get(ActivateTag{});
            (void)(io.get()->*activate_ptr)();

            (void)io->Spawn([](IO& rt) noexcept {
                (void)rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));
            });

            std::thread t([io_ptr = io.release()]() { delete io_ptr; });
            t.join();
        },
        "");
}

// 5. syscall_returns_ecanceled
TEST(ShutdownCancellationTest, SyscallReturnsEcanceled) {
    IoOptions opts;
    IO io(0, opts);
    std::stop_source ss;

    bool fiber_finished = false;
    Error yield_err;
    Error sleep_err;

    auto res = io.Spawn([&](IO& rt) noexcept {
        auto y_res = rt.SleepForNs(std::chrono::nanoseconds(100'000'000ULL));  // 100ms
        if (!y_res) {
            yield_err = y_res.error();
        }

        auto s_res = rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));  // 10s
        if (!s_res) {
            sleep_err = s_res.error();
        }

        fiber_finished = true;
    });
    ASSERT_TRUE(res.has_value());

    std::thread t([&ss]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ss.request_stop();
    });

    io.Run(ss.get_token());
    t.join();

    EXPECT_TRUE(fiber_finished);
    EXPECT_EQ(yield_err.Value(), ECANCELED);
    EXPECT_EQ(sleep_err.Value(), ECANCELED);
}

// 6. double_run
TEST(ShutdownCancellationTest, DoubleRun) {
    IoOptions opts;
    IO io(0, opts);

    bool pass1 = false;
    (void)io.Spawn([&](IO&) noexcept { pass1 = true; });
    io.RunUntilDone();
    EXPECT_TRUE(pass1);

    bool pass2 = false;
    (void)io.Spawn([&](IO&) noexcept { pass2 = true; });
    io.RunUntilDone();
    EXPECT_TRUE(pass2);
}

// Shutdown must not hang. Kills the process if it is still alive after 5s,
// so a regression fails fast instead of waiting for the ctest timeout.
constexpr int k_hung_exit_code = 99;

class ShutdownWatchdog {
public:
    ShutdownWatchdog()
        : t_([this] {
              for (int i = 0; i < 50 && !done_; ++i) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }
              if (!done_) {
                  std::fputs("shutdown hung for 5s\n", stderr);
                  ::_exit(k_hung_exit_code);
              }
          }) {}
    ~ShutdownWatchdog() {
        done_ = true;
        t_.join();
    }

private:
    std::atomic<bool> done_{false};
    std::thread t_;
};

static void RunAndStopAfter50ms(IO& io) {
    std::stop_source ss;
    std::thread t([&ss]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ss.request_stop();
    });
    ShutdownWatchdog watchdog;
    io.Run(ss.get_token());
    t.join();
}

// The runtime's FATAL path exits through easylog's handler, which may turn
// the abort into a plain non-zero exit: accept any death but a hang.
static bool DiedWithoutHanging(const int status) {
    return !WIFEXITED(status) ||
           (WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != k_hung_exit_code);
}

// 7. A fiber parked on a sync primitive nobody will signal is a deadlock:
// once the I/O fibers are cancelled the runtime quiesces and aborts. It used
// to block forever on the eventfd.
TEST(AbandonedParkTest, StopAbortsInsteadOfHanging) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            IO io(0, IoOptions{});
            Latch never(io, 1);
            (void)io.Spawn([&](IO&) noexcept { (void)never.Wait(); });
            (void)io.Spawn([](IO& rt) noexcept {
                (void)rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));  // 10s
            });
            RunAndStopAfter50ms(io);
        },
        DiedWithoutHanging, "");
}

TEST(AbandonedParkTest, RunUntilDoneAbortsInsteadOfHanging) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            IO io(0, IoOptions{});
            Latch never(io, 1);
            (void)io.Spawn([&](IO&) noexcept { (void)never.Wait(); });
            ShutdownWatchdog watchdog;
            io.RunUntilDone();
        },
        DiedWithoutHanging, "");
}

// 7b. The shutdown abort names the primitive the fiber is stuck on. The
// reason lives in the fiber that is parked, so there is no counter to keep
// and the report does not have to guess.
TEST(ShutdownCancellationTest, AbortNamesTheStuckPrimitive) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            // easylog writes the fatal record to stdout; a death test captures
            // stderr.
            (void)::dup2(STDERR_FILENO, STDOUT_FILENO);
            IoOptions opts;
            IO io(0, opts);
            Mutex mu(io);
            // The holder takes the mutex and never gives it back.
            (void)io.Spawn([&](IO& rt) noexcept {
                (void)mu.Lock();
                (void)rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));
            });
            // The waiter parks on the mutex, and nobody will ever wake it.
            (void)io.Spawn([&](IO&) noexcept { (void)mu.Lock(); });
            RunAndStopAfter50ms(io);
        },
        DiedWithoutHanging, "1 on Mutex \\(fiber [0-9]+\\)");
}

// A pool with a single slot: the holder takes it and the waiter parks.
// Built by a function because a designated initializer has a comma, and the
// preprocessor would read it as another macro argument.
static BufferPoolConfig OneSlotPool() { return BufferPoolConfig{.slot_size = 4096, .slots = 1}; }

// Two primitives stuck at once are reported separately, each with its fiber
// ids: the pool waiter and the latch waiter are told apart.
TEST(ShutdownCancellationTest, AbortNamesEveryStuckPrimitive) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            (void)::dup2(STDERR_FILENO, STDOUT_FILENO);
            IoOptions opts;
            opts.buffer_pool = OneSlotPool();
            IO io(0, opts);
            Latch never(io, 1);
            // Holds the only slot, then parks on the latch forever.
            (void)io.Spawn([&](IO& rt) noexcept {
                auto buf = rt.TakeFixedBuffer();
                ASSERT_TRUE(buf.has_value());
                (void)never.Wait();
            });
            // Parks on the pool: the slot is never released.
            (void)io.Spawn([&](IO& rt) noexcept { (void)rt.AcquireFixedBuffer(); });
            RunAndStopAfter50ms(io);
        },
        DiedWithoutHanging,
        "1 on Latch \\(fiber [0-9]+\\), 1 on FixedBufferPool \\(fiber [0-9]+\\)");
}

// 8. A Mutex handed over during shutdown is owned by the woken fiber: Lock()
// succeeds, so the waiter unlocks and the mutex ends up free.
TEST(ShutdownCancellationTest, MutexHandoffDuringShutdownSucceeds) {
    IoOptions opts;
    IO io(0, opts);
    Mutex mu(io);
    bool waiter_locked = false;

    (void)io.Spawn([&](IO& rt) noexcept {
        (void)mu.Lock();
        (void)rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));  // 10s
        mu.Unlock();
    });
    (void)io.Spawn([&](IO&) noexcept {
        if (mu.Lock()) {
            waiter_locked = true;
            mu.Unlock();
        }
    });

    RunAndStopAfter50ms(io);

    EXPECT_TRUE(waiter_locked);
    EXPECT_TRUE(mu.TryLock());
    mu.Unlock();
}

// 9. A parent waiting on a TaskGroup is woken by its cancelled children, not
// by the runtime, and leaves its scope with the group empty.
TEST(ShutdownCancellationTest, TaskGroupParentWokenByCancelledChildren) {
    IoOptions opts;
    IO io(0, opts);
    int cancelled_children = 0;
    bool parent_done = false;

    (void)io.Spawn([&](IO& rt) noexcept {
        TaskGroup group(rt);
        for (int i = 0; i < 3; ++i) {
            (void)group.Spawn([&](IO& child) noexcept {
                auto res = child.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));  // 10s
                if (!res && res.error().Value() == ECANCELED) {
                    ++cancelled_children;
                }
            });
        }
        (void)group.Wait();
        parent_done = true;
    });

    RunAndStopAfter50ms(io);

    EXPECT_EQ(cancelled_children, 3);
    EXPECT_TRUE(parent_done);
    EXPECT_FALSE(io.HasLiveFibers());
}
