//! Fiber stacks: one `mmap` of a `PROT_NONE` guard region below a read/write
//! region. Port of the C++ `detail/stack.cc`.
//!
//! A `Stack` is plain data (a description of a mapping). It has no `Drop`:
//! the fiber header that records it lives inside the mapping, so the owner
//! copies the description out and calls [`Stack::unmap`] last. A stack is
//! unmapped only when its fiber is `Done` and switched out (or never started).

use std::io;
use std::ptr;

/// The system page size.
pub(crate) fn page_size() -> usize {
    // SAFETY: sysconf has no preconditions; _SC_PAGESIZE is a valid name.
    let p = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    if p > 0 { p as usize } else { 4096 }
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct Stack {
    mapping: *mut u8,
    mapping_size: usize,
    high: *mut u8,
}

impl Stack {
    /// A description that maps nothing, for headers built without a stack.
    #[cfg(test)]
    pub(crate) const fn none() -> Stack {
        Stack {
            mapping: ptr::null_mut(),
            mapping_size: 0,
            high: ptr::null_mut(),
        }
    }

    /// Map `guard_size` bytes of `PROT_NONE` followed by `usable_size`
    /// bytes of read/write memory. Both must already be page multiples
    /// (`Options::validate` guarantees it).
    pub(crate) fn map(usable_size: usize, guard_size: usize) -> io::Result<Stack> {
        let mapping_size = guard_size
            .checked_add(usable_size)
            .ok_or_else(|| io::Error::from_raw_os_error(libc::ENOMEM))?;
        let flags = libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_STACK;
        // SAFETY: anonymous private mapping with a null hint: no existing
        // memory is affected; the result is checked below.
        let mapping =
            unsafe { libc::mmap(ptr::null_mut(), mapping_size, libc::PROT_NONE, flags, -1, 0) };
        if mapping == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        let base = mapping.cast::<u8>();
        let low = base.wrapping_add(guard_size);
        // SAFETY: [low, low+usable_size) lies inside the mapping just created.
        let rc =
            unsafe { libc::mprotect(low.cast(), usable_size, libc::PROT_READ | libc::PROT_WRITE) };
        if rc != 0 {
            let err = io::Error::last_os_error();
            // SAFETY: unmapping the mapping we just created and never shared.
            unsafe { libc::munmap(mapping, mapping_size) };
            return Err(err);
        }
        Ok(Stack {
            mapping: base,
            mapping_size,
            high: low.wrapping_add(usable_size),
        })
    }

    /// One past the highest usable address.
    pub(crate) fn high(&self) -> *mut u8 {
        self.high
    }

    /// Give the mapping back to the OS.
    ///
    /// # Safety
    /// Nothing may run on, or point into, this stack afterwards. In
    /// particular the fiber header stored in it is gone.
    pub(crate) unsafe fn unmap(self) {
        if self.mapping.is_null() {
            return;
        }
        // SAFETY: (mapping, mapping_size) is exactly what `map` returned; the
        // caller guarantees nothing uses it any more.
        unsafe { libc::munmap(self.mapping.cast(), self.mapping_size) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[cfg_attr(miri, ignore)] // Miri cannot mmap PROT_NONE
    fn map_gives_usable_region_and_unmaps() {
        let page = page_size();
        let s = Stack::map(4 * page, page).unwrap();
        // SAFETY: the region is mapped read/write; we touch its first and last byte.
        unsafe {
            let low = s.high().sub(4 * page);
            low.write(1);
            s.high().sub(1).write(2);
            assert_eq!(low.read(), 1);
            s.unmap();
        }
    }

    #[test]
    fn page_size_is_a_power_of_two() {
        assert!(page_size().is_power_of_two());
    }
}
