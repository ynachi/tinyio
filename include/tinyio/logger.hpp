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
    #define LOG_BUILD_LEVEL 0
#endif

namespace tinyio::log
{

enum class Level : uint8_t
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Fatal = 4,
    Disabled = 5
};

inline constexpr auto kBuildLevel = static_cast<Level>(LOG_BUILD_LEVEL);
inline std::atomic g_runtime_level{Level::Info};
inline std::atomic g_colors{true};

inline bool should_log(const Level l) noexcept
{
    return l >= g_runtime_level.load(std::memory_order_relaxed);
}

namespace detail
{
struct Record
{
    Level level{Level::Info};
    std::source_location loc;
    std::chrono::system_clock::time_point ts;
    std::thread::id tid;
    uint16_t len{};
    std::array<char, 1024> msg{};
};

inline std::mutex g_mtx;
inline std::counting_semaphore<1024> g_sem{0};
inline std::atomic g_stop{false};
inline std::vector<Record> g_queue;
inline std::vector<Record> g_worker_queue;
inline std::jthread g_worker;

[[nodiscard]] constexpr const char* basename(const char* path) noexcept
{
    const char* last = path;
    for (const char* p = path; p && *p; ++p)
        if (*p == '/')
            last = p + 1;
    return last;
}

// CLOCK_REALTIME_COARSE: ~2ns vs ~20ns for system_clock::now().
// Millisecond resolution is more than enough for log timestamps.
inline std::chrono::system_clock::time_point now_coarse() noexcept
{
#if defined(__linux__)
    timespec ts{};
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    using namespace std::chrono;
    return system_clock::time_point{
        duration_cast<system_clock::duration>(seconds{ts.tv_sec} + nanoseconds{ts.tv_nsec})};
#else
    return std::chrono::system_clock::now();
#endif
}

inline void worker_loop()
{
    while (true)
    {
        g_sem.acquire();

        {
            std::scoped_lock lock(g_mtx);
            // If stopping and everything is drained, exit
            if (g_stop.load(std::memory_order_relaxed) && g_queue.empty())
                break;
            // Instantly swap the queues to release the lock!
            g_worker_queue.swap(g_queue);
        }

        for (const auto& r : g_worker_queue)
        {
            std::string_view color = "", level_str = "UNKN", reset = "";
            if (g_colors.load(std::memory_order_relaxed))
            {
                reset = "\033[0m";
                switch (r.level)
                {
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
            }
            else
            {
                switch (r.level)
                {
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
                        level_str = "ERR";
                        break;
                    case Level::Fatal:
                        level_str = "FTL";
                        break;
                    default:
                        break;
                }
            }

            std::println(stderr, "{}[{}] [{:%H:%M:%S}] [{}] {}:{} | {}{}", color, level_str, r.ts, r.tid,
                         basename(r.loc.file_name()), r.loc.line(), std::string_view(r.msg.data(), r.len), reset);
        }
        g_worker_queue.clear();  // Keeps vector capacity to avoid allocations

        // Re-arm: items or a stop signal that arrived while we were processing
        // must not be silently swallowed — release once so the loop wakes again.
        {
            std::scoped_lock lock(g_mtx);
            if (!g_queue.empty() || g_stop.load(std::memory_order_relaxed))
                g_sem.release();
        }
    }
}

inline void start()
{
    static std::once_flag flag;
    std::call_once(flag,
                   []
                   {
                       // Pre-allocate to prevent heap allocations during runtime
                       g_queue.reserve(1024);
                       g_worker_queue.reserve(1024);
                       g_worker = std::jthread(worker_loop);
                   });
}
}  // namespace detail

inline void set_level(const Level level) noexcept
{
    g_runtime_level.store(level, std::memory_order_relaxed);
}
inline void set_colors(bool enable) noexcept
{
    g_colors.store(enable, std::memory_order_relaxed);
}

inline void stop()
{
    // No lock needed: g_stop is atomic, and the semaphore is self-synchronizing.
    // The release guarantees the worker sees the store (happens-before through acquire).
    detail::g_stop.store(true, std::memory_order_relaxed);
    detail::g_sem.release();
}

inline struct AutoStop
{
    ~AutoStop() { stop(); }
} g_auto_stop;

template <Level L, typename... Args>
void log_impl(const std::source_location loc, std::format_string<Args...> fmt, Args&&... args) noexcept
{
    detail::start();

    // FORMAT ON THE STACK
    // We format completely outside the lock. A slow format operation
    // will no longer stall other threads or the consumer.
    detail::Record r;
    r.level = L;
    r.loc = loc;
    r.ts = detail::now_coarse();
    r.tid = std::this_thread::get_id();

    try
    {
        auto res = std::format_to_n(r.msg.data(), r.msg.size(), fmt, std::forward<Args>(args)...);
        r.len = static_cast<uint16_t>(std::min<size_t>(res.size, r.msg.size()));
    }
    catch (...)
    {
        r.len = 0;
    }

    bool notify = false;

    {
        std::scoped_lock lock(detail::g_mtx);

        if (detail::g_queue.size() < 1024)
        {
            detail::g_queue.push_back(std::move(r));
            // Only release the semaphore if the queue transitioned from empty to 1
            notify = (detail::g_queue.size() == 1);
        }
        // If >= 1024, the message is instantly dropped. No spinning!
    }

    if (notify)
        detail::g_sem.release();
}

}  // namespace tinyio::log

#define ALOG_DETAIL_WRITE(LEVEL, ...)                                                                               \
    do                                                                                                              \
    {                                                                                                               \
        if constexpr (::tinyio::log::Level::LEVEL >= ::tinyio::log::kBuildLevel)                                    \
        {                                                                                                           \
            if (::tinyio::log::should_log(::tinyio::log::Level::LEVEL))                                             \
            {                                                                                                       \
                ::tinyio::log::log_impl<::tinyio::log::Level::LEVEL>(std::source_location::current(), __VA_ARGS__); \
            }                                                                                                       \
        }                                                                                                           \
    } while (0)

#define ALOG_DEBUG(...) ALOG_DETAIL_WRITE(Debug, __VA_ARGS__)
#define ALOG_INFO(...)  ALOG_DETAIL_WRITE(Info, __VA_ARGS__)
#define ALOG_WARN(...)  ALOG_DETAIL_WRITE(Warn, __VA_ARGS__)
#define ALOG_ERROR(...) ALOG_DETAIL_WRITE(Error, __VA_ARGS__)
#define ALOG_FATAL(...) ALOG_DETAIL_WRITE(Fatal, __VA_ARGS__)