#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "../../src/tinyio/runtime.hpp"

using namespace tinyio;

// Helper to access private members of IO for testing
template <typename Tag, typename Tag::type M>
struct Accessor
{
    friend Tag::type get(Tag) { return M; }
};

// Group B — Shutdown and cancellation tests

// 1. stop_token_cancels_waiting
TEST(ShutdownCancellationTest, StopTokenCancelsWaiting)
{
    IoOptions opts;
    IO io(0, opts, {});
    std::stop_source ss;

    bool fiber_ran = false;
    Error fiber_err;

    auto res = io.Spawn(
        [&](IO& rt) noexcept
        {
            fiber_ran = true;
            auto sleep_res = rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));  // 10s
            if (!sleep_res)
            {
                fiber_err = sleep_res.error();
            }
        });
    ASSERT_TRUE(res.has_value());

    auto start = std::chrono::steady_clock::now();

    std::thread t(
        [&ss]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ss.request_stop();
        });

    io.Run(ss.get_token());
    t.join();

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_TRUE(fiber_ran);
    EXPECT_EQ(fiber_err.code, ECANCELED);
    EXPECT_LT(elapsed.count(), 2000);
}

// 2. stop_token_cancels_ready
TEST(ShutdownCancellationTest, StopTokenCancelsReady)
{
    IoOptions opts;
    IO io(0, opts, {});
    std::stop_source ss;

    bool fiber_started = false;
    bool fiber_finished = false;
    tinyio::Error yield_err;

    auto res = io.Spawn(
        [&](IO& rt) noexcept
        {
            fiber_started = true;
            while (true)
            {
                auto yield_res = rt.Yield();
                if (!yield_res)
                {
                    yield_err = yield_res.error();
                    break;
                }
            }
            fiber_finished = true;
        });
    ASSERT_TRUE(res.has_value());

    std::thread t(
        [&ss]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ss.request_stop();
        });

    io.Run(ss.get_token());
    t.join();

    EXPECT_TRUE(fiber_started);
    EXPECT_TRUE(fiber_finished);
    EXPECT_EQ(yield_err.code, ECANCELED);
}

// 3. destructor_cancels_live_work
TEST(ShutdownCancellationTest, DestructorCancelsLiveWork)
{
    IoOptions opts;
    {
        IO io(0, opts, {});
        auto res = io.Spawn(
            [](IO& rt) noexcept
            {
                (void)rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));  // 10s
            });
        ASSERT_TRUE(res.has_value());
    }
}

// Accessor for activate()
struct ActivateTag
{
    using type = Result<void> (IO::*)() noexcept;
};
template struct Accessor<ActivateTag, &IO::Activate>;
Result<void> (IO::* get(ActivateTag))() noexcept;

// 4. destructor_aborts_wrong_thread
TEST(ShutdownCancellationTest, DestructorAbortsWrongThread)
{
    IoOptions opts;
    EXPECT_DEATH(
        {
            auto io = std::make_unique<IO>(0, opts, std::vector<tinyio::BucketConfig>{});

            auto activate_ptr = get(ActivateTag{});
            (void)(io.get()->*activate_ptr)();

            (void)io->Spawn([](IO& rt) noexcept
                            { (void)rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL)); });

            std::thread t([io_ptr = io.release()]() { delete io_ptr; });
            t.join();
        },
        "");
}

// 5. syscall_returns_ecanceled
TEST(ShutdownCancellationTest, SyscallReturnsEcanceled)
{
    IoOptions opts;
    IO io(0, opts, {});
    std::stop_source ss;

    bool fiber_finished = false;
    Error yield_err;
    Error sleep_err;

    auto res = io.Spawn(
        [&](IO& rt) noexcept
        {
            auto y_res = rt.SleepForNs(std::chrono::nanoseconds(100'000'000ULL));  // 100ms
            if (!y_res)
            {
                yield_err = y_res.error();
            }

            auto s_res = rt.SleepForNs(std::chrono::nanoseconds(10'000'000'000ULL));  // 10s
            if (!s_res)
            {
                sleep_err = s_res.error();
            }

            fiber_finished = true;
        });
    ASSERT_TRUE(res.has_value());

    std::thread t(
        [&ss]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ss.request_stop();
        });

    io.Run(ss.get_token());
    t.join();

    EXPECT_TRUE(fiber_finished);
    EXPECT_EQ(yield_err.code, ECANCELED);
    EXPECT_EQ(sleep_err.code, ECANCELED);
}

// 6. double_run
TEST(ShutdownCancellationTest, DoubleRun)
{
    IoOptions opts;
    IO io(0, opts, {});

    bool pass1 = false;
    (void)io.Spawn([&](IO&) noexcept { pass1 = true; });
    io.RunUntilDone();
    EXPECT_TRUE(pass1);

    bool pass2 = false;
    (void)io.Spawn([&](IO&) noexcept { pass2 = true; });
    io.RunUntilDone();
    EXPECT_TRUE(pass2);
}
