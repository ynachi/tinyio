#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tinyio/runtime/runtime.hpp"

using namespace tinyio;

// Group C — Cross-thread post() tests

// 1. post_before_run
TEST(CrossThreadPostTest, PostBeforeRun) {
  IoOptions opts;
  IO io(0, opts);

  bool task_ran = false;

  bool posted = io.Post([&](IO&) noexcept { task_ran = true; });

  ASSERT_TRUE(posted);
  io.RunUntilDone();

  EXPECT_TRUE(task_ran);
}

// 2. post_wakes_sleeping_thread
TEST(CrossThreadPostTest, PostWakesSleepingThread) {
  IoOptions opts;

  std::atomic thread_started{false};
  std::atomic task_ran{false};

  std::stop_source ss;
  IO* io_ptr = nullptr;

  std::thread t([&]() {
    IO io(0, opts);
    io_ptr = &io;
    thread_started = true;
    // Since there are no fibers, Run() will just wait for eventfd wakeup.
    io.Run(ss.get_token());
  });

  while (!thread_started) {
    std::this_thread::yield();
  }

  // Give it a tiny bit to enter io_uring_submit_and_wait
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  const auto start = std::chrono::steady_clock::now();
  const bool posted = io_ptr->Post([&](IO&) noexcept {
    task_ran = true;
    (void)ss.request_stop();  // Task wakes up and stops the loop
  });
  ASSERT_TRUE(posted);

  t.join();

  const auto end = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  EXPECT_TRUE(task_ran);
  EXPECT_LT(elapsed.count(), 100);
}

// 3. post_concurrent_correctness
TEST(CrossThreadPostTest, PostConcurrentCorrectness) {
  IoOptions opts;
  IO io(0, opts);

  std::atomic counter{0};
  constexpr int num_threads = 8;
  constexpr int tasks_per_thread = 1000;

  std::vector<std::thread> threads;

  std::atomic<bool> start_signal{false};

  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&]() {
      while (!start_signal) {
        std::this_thread::yield();
      }
      for (int j = 0; j < tasks_per_thread; ++j) {
        while (!io.Post(
            [&](IO&) noexcept { counter.fetch_add(1, std::memory_order_relaxed); })) {
          // spin if queue is full
          std::this_thread::yield();
        }
      }
    });
  }

  // Start producer threads
  start_signal = true;

  // Also post a task that waits for completion, so the event loop doesn't exit early
  // before the threads are done enqueuing.
  (void)io.Spawn([&](IO& rt) noexcept {
    while (counter.load(std::memory_order_relaxed) < num_threads * tasks_per_thread) {
      (void)rt.SleepForNs(std::chrono::nanoseconds(1'000'000ULL));  // 1ms
    }
  });

  io.RunUntilDone();

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(counter.load(), num_threads * tasks_per_thread);
}

// 4. post_while_fibers_active
TEST(CrossThreadPostTest, PostWhileFibersActive) {
  IoOptions opts;
  // Lower batch_max_size to ensure we interleave post drains with fiber resumes
  opts.batch_max_size = 10;
  IO io(0, opts);

  std::atomic fiber_yields{0};
  std::atomic tasks_ran{0};

  for (int i = 0; i < 10; ++i) {
    (void)io.Spawn([&](IO& rt) noexcept {
      for (int j = 0; j < 100; ++j) {
        (void)rt.Yield();
        ++fiber_yields;
      }
    });
  }

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < 100; ++j) {
        while (!io.Post([&](IO&) noexcept { tasks_ran++; })) {
          std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    });
  }

  std::stop_source ss;

  // Create a watchdog fiber to stop the loop when everything is done
  (void)io.Spawn([&](IO& rt) noexcept {
    while (tasks_ran.load() < 400 || fiber_yields.load() < 1000) {
      (void)rt.SleepForNs(std::chrono::milliseconds(1));
    }
    (void)ss.request_stop();
  });

  io.Run(ss.get_token());

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(fiber_yields.load(), 1000);
  EXPECT_EQ(tasks_ran.load(), 400);
}

// 5. post_after_stop
TEST(CrossThreadPostTest, PostAfterStop) {
  bool destructed = false;

  {
    IoOptions opts;
    IO io(0, opts);
    std::stop_source ss;
    (void)ss.request_stop();
    io.Run(ss.get_token());

    bool posted = io.Post([&](IO&) noexcept {
      // Task logic doesn't run, but its captured state should be destructed
    });
    EXPECT_FALSE(posted);

    // We capture a shared_ptr with a custom deleter to track destruction
    auto tracker = std::shared_ptr<int>(new int(42), [&](int* p) {
      destructed = true;
      delete p;
    });

    posted = io.Post([tracker](IO&) noexcept {});
    EXPECT_FALSE(posted);
    // Let IO destruct
  }

  // Destructor of IO should have drained the post queue and destroyed the tasks
  // Wait, since Post() returns false now, the tracker is destroyed locally immediately!
  EXPECT_TRUE(destructed);
}

// 6. post_queue_full
TEST(CrossThreadPostTest, PostQueueFull) {
  IoOptions opts;
  opts.post_queue_capacity = 16;
  IO io(0, opts);

  int success_count = 0;
  for (int i = 0; i < 100; ++i) {
    if (io.Post([](IO&) noexcept {})) {
      success_count++;
    }
  }

  // With moodycamel::ConcurrentQueue, capacity is a hint.
  // try_enqueue does not allocate, so it will eventually fail.
  // But blocks are size 32, so capacity 16 usually yields 32 slots.
  EXPECT_GE(success_count, 16);
  EXPECT_LT(success_count, 100);
}
