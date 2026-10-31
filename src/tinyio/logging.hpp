/// \file logging.hpp
/// \brief easylog for tinyio: the one place that includes it.
///
/// tinyio logs through YLT's easylog and uses its macros directly at the call
/// sites (ELOGFMT, ELOG, ...). This header takes care of the two things those
/// macros cannot do for themselves: pinning the format backend and bringing
/// easylog up. Include it instead of <ylt/easylog.hpp>.
#pragma once

// easylog picks its ELOGFMT backend with __has_include(<fmt/format.h>): fmt
// when the host has it installed, std::format otherwise. The backend would
// then depend on the machine, and the fmt that ships with Ubuntu (10.1.1) does
// not even compile with clang 21 in the header-only mode easylog forces on it.
// tinyio only needs std::format, so it is pinned here:
//
//   * easylog includes <fmt/format.h> only when FMT_HEADER_ONLY is not
//     defined yet, so defining it around the include keeps the host's fmt out
//     of the translation unit. It is undefined again afterwards, so code that
//     includes fmt after tinyio gets the mode it asks for.
//   * ELOGFMT expands to ELOGFMT_IMPL at the call site, so redefining
//     ELOGFMT_IMPL on top of easylog's ELOGFMT_IMPL0 switches every call to
//     std::format.
//
// Both rely on easylog 0.6.1's internals; the #error catches a YLT upgrade
// that renames them. std::format checks its format string at compile time, so
// ELOGFMT needs a literal one.
#ifndef FMT_HEADER_ONLY
#    define FMT_HEADER_ONLY
#    define TINYIO_UNDEF_FMT_HEADER_ONLY
#endif

#include <ylt/easylog.hpp>

#ifdef TINYIO_UNDEF_FMT_HEADER_ONLY
#    undef FMT_HEADER_ONLY
#    undef TINYIO_UNDEF_FMT_HEADER_ONLY
#endif

#ifndef ELOGFMT_IMPL0
#    error "easylog lost ELOGFMT_IMPL0: revisit the std::format pin in tinyio/logging.hpp"
#endif
#undef ELOGFMT_IMPL
#define ELOGFMT_IMPL(severity, Id, ...) ELOGFMT_IMPL0(severity, Id, std, __VA_ARGS__)

namespace tinyio::detail {

// easylog 0.6.1 constructs its appender without starting the writer thread,
// so a record ends up in a queue nobody drains and is silently dropped. That
// only stops happening once init_log() has been called, which is why the
// library does it here instead of leaving it to the application.
//
// The defaults match what tinyio used before easylog took over: INFO and above
// on the console, no log file, and synchronous writes. Synchronous means no
// writer thread, no records lost when the process exits and no interleaved
// output. An application that wants easylog's asynchronous mode calls
// easylog::init_log() itself, which replaces this configuration.
//
// An inline variable is used on purpose. It is initialized before main in
// every translation unit that includes this header, so the initialization
// happens even when tinyio is linked as a static library and the linker drops
// the object files that nothing references.
inline const bool kEasylogInitialized = [] {
    easylog::init_log(easylog::Severity::INFO, "", false, true);
    return true;
}();

}  // namespace tinyio::detail
