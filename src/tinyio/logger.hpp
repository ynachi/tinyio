#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <format>
#include <mutex>
#include <print>
#include <semaphore>
#include <source_location>
#include <string_view>
#include <thread>
#include <vector>

#ifndef LOG_BUILD_LEVEL
#  define LOG_BUILD_LEVEL 0
#endif

namespace tinyio::log {

enum class Level : uint8_t {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
  Fatal = 4,
  Disabled = 5
};

inline constexpr auto k_build_level = static_cast<Level>(LOG_BUILD_LEVEL);
inline std::atomic g_runtime_level{Level::Info};
inline std::atomic g_colors{true};

inline bool ShouldLog(const Level l) noexcept {
  return l >= g_runtime_level.load(std::memory_order_relaxed);
}

namespace detail {
struct Record {
  Level level{Level::Info};
  std::source_location loc;
  std::chrono::system_clock::time_point ts;
  std::thread::id tid;
  uint16_t len{};
  std::array<char, 1024> msg{};
};

/// All mutable logger state lives in a single struct so that destruction
/// order is deterministic: the jthread joins before the queues and mutex
/// are destroyed, regardless of which TU the linker picks for the inline
/// variable.
struct LoggerState {
  std::mutex mtx;
  std::counting_semaphore<1024> sem{0};
  std::atomic<bool> stop{false};
  std::vector<Record> queue;
  std::vector<Record> worker_queue;
  std::jthread worker;  // destroyed first → joins before fields below are gone

  ~LoggerState() {
    // Signal the worker and let jthread destructor join it.
    stop.store(true, std::memory_order_relaxed);
    sem.release();
  }
};

inline LoggerState& MakeloggerState() {
  static LoggerState s;
  return s;
}

[[nodiscard]] constexpr const char* Basename(const char* path) noexcept {
  const char* last = path;
  for (const char* p = path; p && *p; ++p)
    if (*p == '/') last = p + 1;
  return last;
}

// CLOCK_REALTIME_COARSE: ~2ns vs ~20ns for system_clock::now().
// Millisecond resolution is more than enough for log timestamps.
inline std::chrono::system_clock::time_point NowCoarse() noexcept {
#if defined(__linux__)
  timespec ts{};
  clock_gettime(CLOCK_REALTIME_COARSE, &ts);
  using namespace std::chrono;
  return system_clock::time_point{duration_cast<system_clock::duration>(
      seconds{ts.tv_sec} + nanoseconds{ts.tv_nsec})};
#else
  return std::chrono::system_clock::now();
#endif
}

inline void WorkerLoop() {
  auto& st = MakeloggerState();
  while (true) {
    st.sem.acquire();

    {
      std::scoped_lock lock(st.mtx);
      // If stopping and everything is drained, exit
      if (st.stop.load(std::memory_order_relaxed) && st.queue.empty()) break;
      // Instantly swap the queues to release the lock!
      st.worker_queue.swap(st.queue);
    }

    for (const auto& r : st.worker_queue) {
      std::string_view color = "";
      std::string_view level_str = "UNKN";
      std::string_view reset = "";
      if (g_colors.load(std::memory_order_relaxed)) {
        reset = "\033[0m";
        switch (r.level) {
          case Level::Debug:
            color = "\033[36m";
            level_str = "DBG";
            break;
          case Level::Info:
            color = "\033[32m";
            level_str = "INF";
            break;
          case Level::Warn:
            color = "\033[33m";
            level_str = "WRN";
            break;
          case Level::Error:
            color = "\033[31m";
            level_str = "ERR";
            break;
          case Level::Fatal:
            color = "\033[1;31m";
            level_str = "FTL";
            break;
          default:
            break;
        }
      } else {
        switch (r.level) {
          case Level::Debug:
            level_str = "DBG";
            break;
          case Level::Info:
            level_str = "INF";
            break;
          case Level::Warn:
            level_str = "WRN";
            break;
          case Level::Error:
            color = "\033[31m";
            level_str = "ERR";
            break;
          case Level::Fatal:
            level_str = "FTL";
            break;
          default:
            break;
        }
      }

      std::println(stderr, "{}[{}] [{:%H:%M:%S}] [{}] {}:{} | {}{}", color, level_str,
                   r.ts, r.tid, Basename(r.loc.file_name()), r.loc.line(),
                   std::string_view(r.msg.data(), r.len), reset);
    }
    st.worker_queue.clear();  // Keeps vector capacity to avoid allocations

    // Re-arm: items or a stop signal that arrived while we were processing
    // must not be silently swallowed — release once so the loop wakes again.
    {
      std::scoped_lock lock(st.mtx);
      if (!st.queue.empty() || st.stop.load(std::memory_order_relaxed)) st.sem.release();
    }
  }
}

inline void Start() {
  static std::once_flag flag;
  std::call_once(flag, [] {
    auto& st = MakeloggerState();
    // Pre-allocate to prevent heap allocations during runtime
    st.queue.reserve(1024);
    st.worker_queue.reserve(1024);
    st.worker = std::jthread(WorkerLoop);
  });
}
}  // namespace detail

inline void SetLevel(const Level level) noexcept {
  g_runtime_level.store(level, std::memory_order_relaxed);
}
inline void SetColors(const bool enable) noexcept {
  g_colors.store(enable, std::memory_order_relaxed);
}

inline void Stop() {
  // Triggers LoggerState destructor at program exit (function-local static).
  // The destructor sets the stop flag and releases the semaphore, then the
  // jthread destructor joins the worker thread — all in deterministic order.
  // Calling this explicitly is optional; the static destructor handles it.
  (void)detail::MakeloggerState();
}

template <Level L, typename... Args>
void LogImpl(const std::source_location loc, std::format_string<Args...> fmt,
             Args&&... args) noexcept {
  detail::Start();

  // FORMAT ON THE STACK
  // We format completely outside the lock. A slow format operation
  // will no longer stall other threads or the consumer.
  detail::Record r;
  r.level = L;
  r.loc = loc;
  r.ts = detail::NowCoarse();
  r.tid = std::this_thread::get_id();

  try {
    auto res =
        std::format_to_n(r.msg.data(), r.msg.size(), fmt, std::forward<Args>(args)...);
    r.len = static_cast<uint16_t>(std::min<size_t>(res.size, r.msg.size()));
  } catch (...) {
    r.len = 0;
  }

  auto& st = detail::MakeloggerState();
  bool notify = false;

  {
    std::scoped_lock lock(st.mtx);

    if (st.queue.size() < 1024) {
      st.queue.push_back(std::move(r));
      // Only release the semaphore if the queue transitioned from empty to 1
      notify = (st.queue.size() == 1);
    }
    // If >= 1024, the message is instantly dropped. No spinning!
  }

  if (notify) st.sem.release();
}

}  // namespace tinyio::log

#define ALOG_DETAIL_WRITE(LEVEL, ...)                                            \
  do {                                                                           \
    if constexpr (::tinyio::log::Level::LEVEL >= ::tinyio::log::k_build_level) { \
      if (::tinyio::log::ShouldLog(::tinyio::log::Level::LEVEL)) {               \
        ::tinyio::log::LogImpl<::tinyio::log::Level::LEVEL>(                     \
            std::source_location::current(), __VA_ARGS__);                       \
      }                                                                          \
    }                                                                            \
  } while (0)

#define ALOG_DEBUG(...) ALOG_DETAIL_WRITE(Debug, __VA_ARGS__)
#define ALOG_INFO(...) ALOG_DETAIL_WRITE(Info, __VA_ARGS__)
#define ALOG_WARN(...) ALOG_DETAIL_WRITE(Warn, __VA_ARGS__)
#define ALOG_ERROR(...) ALOG_DETAIL_WRITE(Error, __VA_ARGS__)
#define ALOG_FATAL(...) ALOG_DETAIL_WRITE(Fatal, __VA_ARGS__)