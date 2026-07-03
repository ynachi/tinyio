#pragma once

#include <format>
#include <source_location>
#include <string>

#include <absl/log/log.h>

namespace tinyio::log {

enum class Level : uint8_t {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
  Fatal = 4,
  Disabled = 5
};

inline void SetLevel(const Level /*level*/) noexcept {}
inline void SetColors(const bool /*enable*/) noexcept {}
inline void Stop() {}

template <Level L, typename... Args>
void LogImpl(const std::source_location loc, std::format_string<Args...> fmt,
             Args&&... args) noexcept {
  std::string formatted;
  try {
    formatted = std::format(fmt, std::forward<Args>(args)...);
  } catch (...) {
    formatted = "formatting error";
  }

  // Prepend file and line number
  const auto msg = std::format("[{}:{}] {}", loc.file_name(), loc.line(), formatted);

  if constexpr (L == Level::Debug) {
    DLOG(INFO) << msg;
  } else if constexpr (L == Level::Info) {
    LOG(INFO) << msg;
  } else if constexpr (L == Level::Warn) {
    LOG(WARNING) << msg;
  } else if constexpr (L == Level::Error) {
    LOG(ERROR) << msg;
  } else if constexpr (L == Level::Fatal) {
    LOG(FATAL) << msg;
  }
}

}  // namespace tinyio::log

#ifndef LOG_BUILD_LEVEL
#  define LOG_BUILD_LEVEL 0
#endif
inline constexpr auto k_build_level = static_cast<tinyio::log::Level>(LOG_BUILD_LEVEL);

#define ALOG_DETAIL_WRITE(LEVEL, ...)                             \
  do {                                                            \
    if constexpr (::tinyio::log::Level::LEVEL >= k_build_level) { \
      ::tinyio::log::LogImpl<::tinyio::log::Level::LEVEL>(        \
          std::source_location::current(), __VA_ARGS__);          \
    }                                                             \
  } while (0)

#define ALOG_DEBUG(...) ALOG_DETAIL_WRITE(Debug, __VA_ARGS__)
#define ALOG_INFO(...) ALOG_DETAIL_WRITE(Info, __VA_ARGS__)
#define ALOG_WARN(...) ALOG_DETAIL_WRITE(Warn, __VA_ARGS__)
#define ALOG_ERROR(...) ALOG_DETAIL_WRITE(Error, __VA_ARGS__)
#define ALOG_FATAL(...) ALOG_DETAIL_WRITE(Fatal, __VA_ARGS__)