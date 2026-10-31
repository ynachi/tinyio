#![forbid(unsafe_code)]
//! Rule 8: an internal invariant failure logs and aborts. It never unwinds.
//!
//! Unwinding out of the scheduler would run destructors on a fiber stack the
//! kernel or a parked fiber may still point into, so there is no recovery path
//! to offer: say what broke, then `abort`.

use std::fmt;
use std::io::Write;

/// Print `msg` to stderr and abort the process.
#[cold]
#[inline(never)]
pub(crate) fn fatal(msg: fmt::Arguments<'_>) -> ! {
    // Ignore write errors: we are about to abort anyway.
    let _ = writeln!(std::io::stderr().lock(), "tinyio: fatal: {msg}");
    std::process::abort()
}

/// `check!(cond, "fmt", args..)`: abort with a message when `cond` is false.
/// Always on, including in release builds.
macro_rules! check {
    ($cond:expr, $($arg:tt)+) => {
        if !$cond {
            $crate::check::fatal(format_args!($($arg)+));
        }
    };
}

/// Like `check!` but compiled out of release builds, for hot-path assertions.
macro_rules! dcheck {
    ($cond:expr, $($arg:tt)+) => {
        if cfg!(debug_assertions) && !$cond {
            $crate::check::fatal(format_args!($($arg)+));
        }
    };
}

pub(crate) use {check, dcheck};
