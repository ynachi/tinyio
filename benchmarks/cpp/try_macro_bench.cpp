#include <chrono>
#include <iostream>

#include <ylt/easylog.hpp>

#include "tinyio/error.hpp"

using namespace tinyio;

inline Result<int> dummy_success() { return 42; }

inline Result<int> dummy_error() { return Error::fail_errno(EINVAL, "dummy"); }

inline Result<int> test_try_success() {
  TRY(n, dummy_success());
  return n;
}

inline Result<int> test_try_log_success() {
  TRY_LOG_ERR(n, dummy_success(), "failed");
  return n;
}

// 1. "Without TRY" - ignoring errors entirely (like just doing *dummy_success())
inline int test_ignore_success() { return *dummy_success(); }

// 2. "Go-style" - manual if (!res) error handling
inline Result<int> test_go_style_success() {
  auto res = dummy_success();
  if (!res) {
    return std::unexpected(res.error());
  }
  return *res;
}

// 3. "Go-style with logging"
inline Result<int> test_go_style_log_success() {
  auto res = dummy_success();
  if (!res) {
    const auto err = res.error();
    ELOGFMT(ERROR, "{} failed: code={} failed", err.context(), err.value());
    return std::unexpected(err);
  }
  return *res;
}

inline Result<int> test_try_error() {
  TRY(n, dummy_error());
  return n;
}

inline Result<int> test_try_log_error() {
  TRY_LOG_ERR(n, dummy_error(), "failed");
  return n;
}

inline Result<int> test_go_style_error() {
  auto res = dummy_error();
  if (!res) {
    return std::unexpected(res.error());
  }
  return *res;
}

inline Result<int> test_go_style_log_error() {
  auto res = dummy_error();
  if (!res) {
    const auto err = res.error();
    ELOGFMT(ERROR, "{} failed: code={} failed", err.context(), err.value());
    return std::unexpected(err);
  }
  return *res;
}

// Ensure the compiler doesn't optimize the loop away
void escape(void* p) { asm volatile("" : : "g"(p) : "memory"); }

int main() {
  constexpr size_t ITERATIONS = 10000000;

  std::cout << "--- SUCCESS PATH ---\n";

  {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
      auto res = test_ignore_success();
      escape(&res);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "Ignore Error (YOLO):     " << (double)ns / ITERATIONS << " ns/op\n";
  }

  {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
      auto res = test_go_style_success();
      escape(&res);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "Go-style if(!err):       " << (double)ns / ITERATIONS << " ns/op\n";
  }

  {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
      auto res = test_go_style_log_success();
      escape(&res);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "Go-style log if(!err):   " << (double)ns / ITERATIONS << " ns/op\n";
  }

  {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
      auto res = test_try_success();
      escape(&res);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "TRY success path:        " << (double)ns / ITERATIONS << " ns/op\n";
  }

  {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
      auto res = test_try_log_success();
      escape(&res);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "TRY_LOG_ERR success:     " << (double)ns / ITERATIONS << " ns/op\n";
  }

  std::cout << "\n--- ERROR PATH ---\n";

  constexpr size_t ERR_ITERATIONS = 10000;

  {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ERR_ITERATIONS; ++i) {
      auto res = test_go_style_error();
      escape(&res);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "Go-style if(!err):       " << (double)ns / ERR_ITERATIONS << " ns/op\n";
  }

  {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ERR_ITERATIONS; ++i) {
      auto res = test_go_style_log_error();
      escape(&res);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "Go-style log if(!err):   " << (double)ns / ERR_ITERATIONS << " ns/op\n";
  }

  {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ERR_ITERATIONS; ++i) {
      auto res = test_try_error();
      escape(&res);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "TRY error path:          " << (double)ns / ERR_ITERATIONS << " ns/op\n";
  }

  {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ERR_ITERATIONS; ++i) {
      auto res = test_try_log_error();
      escape(&res);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "TRY_LOG_ERR error:       " << (double)ns / ERR_ITERATIONS << " ns/op\n";
  }

  return 0;
}
