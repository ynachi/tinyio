#![forbid(unsafe_code)]

use std::io;

/// Runtime configuration. Defaults match the C++ `IoOptions`, except the
/// guard region, which is 64 KiB here (the plan, section 6: Rust's stack
/// probes make an overflow a SIGSEGV for any frame they cover, but C and asm
/// frames can skip a small guard).
#[derive(Clone, Debug)]
pub struct Options {
    /// Usable stack bytes per fiber, rounded up to whole pages (minimum 4).
    /// The spawned closure and the fiber header live at the top of it.
    pub stack_size: usize,
    /// `PROT_NONE` bytes below each stack, rounded up to whole pages. Must be
    /// at least one page.
    pub guard_size: usize,
    /// Finished fibers (stack included) kept for reuse; beyond this the stack
    /// goes back to the OS.
    pub max_idle_fibers: usize,
    /// Most fibers resumed per tick before the loop submits and reaps.
    pub batch_max_size: usize,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            stack_size: 256 * 1024,
            guard_size: 64 * 1024,
            max_idle_fibers: 1024,
            batch_max_size: 128,
        }
    }
}

/// An `Options` after validation: sizes rounded to pages, nothing zero.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Validated {
    pub stack_size: usize,
    pub guard_size: usize,
    pub max_idle_fibers: usize,
    pub batch_max_size: usize,
}

use crate::detail::stack::page_size;

impl Options {
    pub(crate) fn validate(&self) -> io::Result<Validated> {
        let page = page_size();
        if self.guard_size == 0 {
            return Err(invalid("guard_size must be at least one page"));
        }
        if self.batch_max_size == 0 {
            return Err(invalid("batch_max_size must be at least 1"));
        }
        let round = |v: usize| {
            v.checked_add(page - 1)
                .map(|x| x & !(page - 1))
                .ok_or_else(|| invalid("size overflows"))
        };
        let stack_size = round(self.stack_size.max(4 * page))?;
        let guard_size = round(self.guard_size)?;
        if stack_size > isize::MAX as usize / 2 || guard_size > isize::MAX as usize / 2 {
            return Err(invalid("size overflows"));
        }
        Ok(Validated {
            stack_size,
            guard_size,
            max_idle_fibers: self.max_idle_fibers,
            batch_max_size: self.batch_max_size,
        })
    }
}

fn invalid(msg: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, msg)
}
