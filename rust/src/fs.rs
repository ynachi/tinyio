#![forbid(unsafe_code)]
//! `OpenOptions` for `Cx::open`. `std::fs::OpenOptions` does not expose the
//! flags it builds, and a raw `i32` of `libc::O_*` bits invites mistakes, so
//! this is a small mirror of it with the same validation rules.

use std::io;

#[derive(Clone, Debug)]
pub struct OpenOptions {
    read: bool,
    write: bool,
    append: bool,
    truncate: bool,
    create: bool,
    create_new: bool,
    mode: u32,
}

impl Default for OpenOptions {
    fn default() -> Self {
        OpenOptions::new()
    }
}

impl OpenOptions {
    /// Nothing set: not yet a valid combination (set `read` or `write`).
    pub fn new() -> OpenOptions {
        OpenOptions {
            read: false,
            write: false,
            append: false,
            truncate: false,
            create: false,
            create_new: false,
            mode: 0o666,
        }
    }

    pub fn read(&mut self, yes: bool) -> &mut Self {
        self.read = yes;
        self
    }

    pub fn write(&mut self, yes: bool) -> &mut Self {
        self.write = yes;
        self
    }

    pub fn append(&mut self, yes: bool) -> &mut Self {
        self.append = yes;
        self
    }

    pub fn truncate(&mut self, yes: bool) -> &mut Self {
        self.truncate = yes;
        self
    }

    pub fn create(&mut self, yes: bool) -> &mut Self {
        self.create = yes;
        self
    }

    pub fn create_new(&mut self, yes: bool) -> &mut Self {
        self.create_new = yes;
        self
    }

    /// Permission bits for a created file (before the umask). Default `0o666`.
    pub fn mode(&mut self, mode: u32) -> &mut Self {
        self.mode = mode;
        self
    }

    pub(crate) fn mode_bits(&self) -> u32 {
        self.mode
    }

    /// The `open(2)` flags, with `O_CLOEXEC` always on. Same validation as
    /// `std::fs::OpenOptions`.
    pub(crate) fn flags(&self) -> io::Result<i32> {
        let invalid = || io::Error::from_raw_os_error(libc::EINVAL);
        let access = match (self.read, self.write, self.append) {
            (true, false, false) => libc::O_RDONLY,
            (false, true, false) => libc::O_WRONLY,
            (true, true, false) => libc::O_RDWR,
            (false, _, true) => libc::O_WRONLY | libc::O_APPEND,
            (true, _, true) => libc::O_RDWR | libc::O_APPEND,
            (false, false, false) => return Err(invalid()),
        };
        match (self.write, self.append) {
            (true, false) => {}
            (false, false) => {
                if self.truncate || self.create || self.create_new {
                    return Err(invalid());
                }
            }
            (_, true) => {
                if self.truncate && !self.create_new {
                    return Err(invalid());
                }
            }
        }
        let creation = match (self.create, self.truncate, self.create_new) {
            (_, _, true) => libc::O_CREAT | libc::O_EXCL,
            (false, false, false) => 0,
            (true, false, false) => libc::O_CREAT,
            (false, true, false) => libc::O_TRUNC,
            (true, true, false) => libc::O_CREAT | libc::O_TRUNC,
        };
        Ok(libc::O_CLOEXEC | access | creation)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn flags(f: impl FnOnce(&mut OpenOptions)) -> io::Result<i32> {
        let mut o = OpenOptions::new();
        f(&mut o);
        o.flags()
    }

    #[test]
    fn read_only_is_the_simple_case() {
        let f = flags(|o| {
            o.read(true);
        })
        .unwrap();
        assert_eq!(f, libc::O_CLOEXEC | libc::O_RDONLY);
    }

    #[test]
    fn create_truncate_write() {
        let f = flags(|o| {
            o.write(true).create(true).truncate(true);
        })
        .unwrap();
        assert_eq!(
            f,
            libc::O_CLOEXEC | libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC
        );
    }

    #[test]
    fn invalid_combinations_are_einval() {
        let einval = |r: io::Result<i32>| r.unwrap_err().raw_os_error() == Some(libc::EINVAL);
        assert!(einval(flags(|_| {})), "no access mode");
        assert!(einval(flags(|o| {
            o.read(true).create(true);
        })));
        assert!(einval(flags(|o| {
            o.append(true).truncate(true);
        })));
    }

    #[test]
    fn create_new_wins() {
        let f = flags(|o| {
            o.write(true).create_new(true).create(true);
        })
        .unwrap();
        assert_eq!(
            f & (libc::O_CREAT | libc::O_EXCL),
            libc::O_CREAT | libc::O_EXCL
        );
    }
}
