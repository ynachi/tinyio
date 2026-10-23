/// \file logging.hpp
/// \brief easylog initialization for tinyio.
///
/// tinyio logs through YLT's easylog and uses its macros directly at the call
/// sites (ELOGFMT, ELOG, ...). This header only takes care of the one thing
/// those macros cannot do for themselves: bringing easylog up.
#pragma once

#include <ylt/easylog.hpp>

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
