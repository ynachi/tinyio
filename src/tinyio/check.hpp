#pragma once

#include "logging.hpp"

// Abseil is gone from tinyio, and with it absl::CHECK. easylog has no
// equivalent of its own, so this is the same contract on top of easylog's
// fatal record: the message carries the file and the line, a stacktrace is
// printed, the log is flushed, and the process exits.
//
// The difference with absl::CHECK is the way the process dies: easylog exits
// with EXIT_FAILURE, absl calls abort() and therefore leaves a core dump.
//
// The macro is opt-in -- it is not pulled in by the other tinyio headers --
// so it cannot collide with abseil's own CHECK in code that includes both.
//
// Like every if-shaped macro, an unbraced else binds to the inner if:
//   if (a) CHECK(cond); else g();  // g() runs when cond held
// Always brace the call site:
//   if (a) { CHECK(cond); } else { g(); }
#ifndef CHECK
#  define CHECK(cond) \
    if (cond) {       \
    } else            \
      ELOG(FATAL) << "Check failed: " #cond << " "
#endif
