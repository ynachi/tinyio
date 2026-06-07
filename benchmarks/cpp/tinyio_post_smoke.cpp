// Cross-thread post() smoke test.
//
// Two scenarios:
//   test_post_before_run  – all tasks posted before run_until_done() is called
//   test_post_while_running – tasks posted while run() is active on the owner thread
//
// Each scenario uses kProducers threads posting kTasksPerProducer tasks.
// A passing run prints "PASS" for each test and exits with code 0.

#include <tinyio/runtime.hpp>
#include <tinyio/logger.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static constexpr int kProducers = 4;
static constexpr int kTasksPerProducer = 2'000;
static constexpr int kTotal = kProducers * kTasksPerProducer;

// -------------------------------------------------------------------------
// Test 1: all posts done before run_until_done()
//
// IO is constructed on the main thread (owner = main).  Producers enqueue
// kTotal tasks via post() and join.  Then main calls run_until_done(),
// which must drain post_queue_, spawn all tasks, and exit cleanly.
// -------------------------------------------------------------------------
static bool test_post_before_run()
{
    std::atomic<int> counter{0};
    tinyio::IoOptions opts;
    tinyio::IO io(0, opts, {});

    {
        std::vector<std::jthread> producers;
        producers.reserve(kProducers);
        for (int t = 0; t < kProducers; ++t)
        {
            producers.emplace_back([&io, &counter] {
                for (int i = 0; i < kTasksPerProducer; ++i)
                {
                    while (!io.post([&counter](tinyio::IO&) noexcept {
                        counter.fetch_add(1, std::memory_order_relaxed);
                    }))
                    {
                        std::this_thread::yield();
                    }
                }
            });
        }
    }  // all producers joined here

    io.run_until_done();

    const int final_count = counter.load(std::memory_order_seq_cst);
    if (final_count != kTotal)
    {
        std::cerr << "[FAIL] test_post_before_run: " << final_count << " / " << kTotal << '\n';
        return false;
    }
    std::cout << "[PASS] test_post_before_run: " << final_count << " tasks\n";
    return true;
}

// -------------------------------------------------------------------------
// Test 2: posts arrive while run() is active
//
// IO MUST be constructed on the runner thread so that owner_tid_ matches
// the thread that calls tick() (and therefore spawn_task()).
// Producers post concurrently.  Main waits for all tasks to execute, then
// requests stop.
// -------------------------------------------------------------------------
static bool test_post_while_running()
{
    std::atomic<int> counter{0};
    std::atomic<tinyio::IO*> io_ptr{nullptr};

    // Construct and run IO on the runner thread so owner_tid_ == runner.
    std::jthread runner([&](std::stop_token st) {
        tinyio::IoOptions opts;
        tinyio::IO io(0, opts, {});
        io_ptr.store(&io, std::memory_order_release);
        io.run(st);
        io_ptr.store(nullptr, std::memory_order_release);
    });

    // Wait until the runner has constructed and published the IO pointer.
    tinyio::IO* io = nullptr;
    while ((io = io_ptr.load(std::memory_order_acquire)) == nullptr)
    {
        std::this_thread::yield();
    }

    // Post from N producer threads while run() is active.
    {
        std::vector<std::jthread> producers;
        producers.reserve(kProducers);
        for (int t = 0; t < kProducers; ++t)
        {
            producers.emplace_back([io, &counter] {
                for (int i = 0; i < kTasksPerProducer; ++i)
                {
                    while (!io->post([&counter](tinyio::IO&) noexcept {
                        counter.fetch_add(1, std::memory_order_relaxed);
                    }))
                    {
                        std::this_thread::yield();
                    }
                }
            });
        }
    }  // all producers joined here

    // Wait for all tasks to execute; bail out after 10 s to surface hangs.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (counter.load(std::memory_order_acquire) < kTotal)
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            std::cerr << "[FAIL] test_post_while_running: timeout, "
                      << counter.load() << " / " << kTotal << '\n';
            runner.request_stop();
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }

    runner.request_stop();
    // std::jthread destructor joins runner.

    const int final_count = counter.load(std::memory_order_seq_cst);
    if (final_count != kTotal)
    {
        std::cerr << "[FAIL] test_post_while_running: " << final_count << " / " << kTotal << '\n';
        return false;
    }
    std::cout << "[PASS] test_post_while_running: " << final_count << " tasks\n";
    return true;
}

int main()
{
    tinyio::log::set_level(tinyio::log::Level::Disabled);

    bool ok = true;
    ok &= test_post_before_run();
    ok &= test_post_while_running();
    return ok ? 0 : 1;
}
