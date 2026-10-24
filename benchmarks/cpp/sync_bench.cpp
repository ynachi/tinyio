#include <chrono>
#include <iostream>

#include "tinyio/runtime/fiber_sync.hpp"
#include "tinyio/runtime/runtime.hpp"

void bench_mutex(tinyio::IO& io, size_t iterations) {
  tinyio::Mutex mutex(io);
  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i) {
    (void)mutex.Lock();
    mutex.Unlock();
  }
  auto end = std::chrono::steady_clock::now();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  std::cout << "Mutex uncontended lock/unlock: " << (double)ns / iterations << " ns/op\n";
}

void bench_semaphore(tinyio::IO& io, size_t iterations) {
  tinyio::Semaphore sem(io, 1);
  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i) {
    (void)sem.Acquire();
    sem.Release();
  }
  auto end = std::chrono::steady_clock::now();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  std::cout << "Semaphore uncontended acquire/release: " << (double)ns / iterations
            << " ns/op\n";
}

void bench_latch(tinyio::IO& io, size_t iterations) {
  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i) {
    tinyio::Latch latch(io, 1);
    latch.CountDown();
  }
  auto end = std::chrono::steady_clock::now();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  std::cout << "Latch create + countdown: " << (double)ns / iterations << " ns/op\n";
}

void bench_taskgroup(tinyio::IO& io, size_t iterations) {
  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i) {
    tinyio::TaskGroup tg(io);
    (void)tg.Spawn([](tinyio::IO&) noexcept {});
    (void)tg.Wait();
  }
  auto end = std::chrono::steady_clock::now();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  std::cout << "TaskGroup spawn + wait: " << (double)ns / iterations << " ns/op\n";
}

void bench_mutex_contended(tinyio::IO& io, size_t iterations) {
  tinyio::Mutex mutex(io);
  tinyio::TaskGroup tg(io);
  auto start = std::chrono::steady_clock::now();

  (void)tg.Spawn([&](tinyio::IO& rt) noexcept {
    for (size_t i = 0; i < iterations; ++i) {
      (void)mutex.Lock();
      mutex.Unlock();
      (void)rt.Yield();  // yield to increase contention chance
    }
  });

  for (size_t i = 0; i < iterations; ++i) {
    (void)mutex.Lock();
    mutex.Unlock();
    (void)io.Yield();
  }
  (void)tg.Wait();

  auto end = std::chrono::steady_clock::now();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  std::cout << "Mutex contended lock/unlock (+yields): " << (double)ns / (iterations * 2)
            << " ns/op\n";
}

int main() {
  tinyio::IoOptions opts;
  tinyio::IO io(0, opts);

  constexpr size_t iterations = 10000000;  // 10 million
  std::stop_source st;

  (void)io.Spawn([&](tinyio::IO& rt) noexcept {
    std::cout << "--- Fiber Sync Primitives Benchmark ---\n";
    bench_mutex(rt, iterations);
    bench_semaphore(rt, iterations);
    bench_latch(rt, iterations);
    bench_taskgroup(rt, iterations / 10);
    bench_mutex_contended(rt, iterations / 10);

    st.request_stop();
  });

  io.Run(st.get_token());
  return 0;
}
