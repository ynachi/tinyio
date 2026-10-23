// tinyio's easylog formatting backend shim.
//
// easylog picks its formatting backend with __has_include(<fmt/format.h>): with
// fmt present it calls fmt::format, without it std::format. Leaving that to the
// host has two consequences tinyio cannot accept:
//
//   * The build stops being reproducible. The CI container ships no fmt, so it
//     builds on std::format, while a developer box that happens to have fmt
//     installed builds on fmt. The same source then produces two different
//     log formats depending on the machine.
//
//   * The fmt that ships with recent Ubuntu (10.1.1) does not compile with
//     clang 21 in header-only mode, which is the mode easylog forces: it
//     defines FMT_HEADER_ONLY before including fmt. The failure is inside
//     fmt's own code, not in anything tinyio calls:
//
//       error: call to consteval function
//       'fmt::basic_format_string<...>::basic_format_string<FMT_COMPILE_STRING, 0>'
//       is not a constant expression
//
// So this header keeps the include path honest - easylog still finds a
// fmt/format.h and still calls fmt::format - while pinning the backend to
// std::format, the implementation the CI container already uses. fmt::format
// is nothing but an alias for std::format here, so no second formatting
// implementation enters the build.
//
// Two consequences worth knowing about:
//
//   * std::format checks the format string at compile time and requires it to
//     be a constant expression. fmt also accepts a runtime string. Every
//     ELOGFMT call site in tinyio passes a literal, but code written against
//     tinyio's build tree must do the same.
//
//   * This file is deliberately not installed. It is a build-time shim, and
//     tinyio's public headers must not shadow the real fmt for consumers.
//
// Set TINYIO_USE_SYSTEM_FMT=ON to drop the shim and let easylog use whatever
// fmt the host provides.
#pragma once

#include <format>

namespace fmt {
using std::format;
}
