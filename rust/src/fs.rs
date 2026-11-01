#![forbid(unsafe_code)]
//! `OpenOptions` for `Cx::open`. `std::fs::OpenOptions` does not expose the
//! flags it builds, and a raw `i32` of `libc::O_*` bits invites mistakes, so
//! this is a small mirror of it with the same validation rules.

use std::io;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

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

/// What `Cx::metadata` reports: the few fields storage code asks for, from one
/// `statx`. (`std::fs::Metadata` cannot be built from a `statx` result.) More
/// are added when a caller needs them.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Metadata {
    len: u64,
    mode: u32,
    modified: SystemTime,
}

impl Metadata {
    pub(crate) fn from_statx(size: u64, mode: u32, mtime_sec: i64, mtime_nsec: u32) -> Metadata {
        let nanos = Duration::from_nanos(u64::from(mtime_nsec));
        let modified = match u64::try_from(mtime_sec) {
            Ok(secs) => UNIX_EPOCH + Duration::from_secs(secs) + nanos,
            // Before 1970: back off by whole seconds, then forward by the fraction.
            Err(_) => UNIX_EPOCH - Duration::from_secs(mtime_sec.unsigned_abs()) + nanos,
        };
        Metadata {
            len: size,
            mode,
            modified,
        }
    }

    /// Size in bytes. (No `is_empty`: like `std::fs::Metadata`, a zero-length
    /// file is still a file.)
    #[allow(clippy::len_without_is_empty)]
    pub fn len(&self) -> u64 {
        self.len
    }

    /// A regular file (as opposed to a directory, symlink, device, ...).
    pub fn is_file(&self) -> bool {
        self.mode & S_IFMT == S_IFREG
    }

    pub fn is_dir(&self) -> bool {
        self.mode & S_IFMT == S_IFDIR
    }

    /// Time of last modification.
    pub fn modified(&self) -> SystemTime {
        self.modified
    }

    /// The `st_mode` bits: file type and permissions.
    pub fn mode(&self) -> u32 {
        self.mode
    }
}

const S_IFMT: u32 = libc::S_IFMT;
const S_IFREG: u32 = libc::S_IFREG;
const S_IFDIR: u32 = libc::S_IFDIR;

#[cfg(test)]
mod metadata_tests {
    use super::*;

    // Prediction: the type bits decide is_file / is_dir, and nothing else does.
    #[test]
    fn type_predicates() {
        let file = Metadata::from_statx(3, S_IFREG | 0o644, 0, 0);
        assert!(file.is_file() && !file.is_dir());
        let dir = Metadata::from_statx(0, S_IFDIR | 0o755, 0, 0);
        assert!(dir.is_dir() && !dir.is_file());
        let other = Metadata::from_statx(0, libc::S_IFLNK | 0o777, 0, 0);
        assert!(!other.is_file() && !other.is_dir());
        assert_eq!(file.len(), 3);
        assert_eq!(file.mode(), S_IFREG | 0o644);
    }

    // Prediction: modification times convert exactly, before the epoch too.
    #[test]
    fn times_convert() {
        let m = Metadata::from_statx(0, 0, 10, 500);
        assert_eq!(m.modified(), UNIX_EPOCH + Duration::new(10, 500));
        let m = Metadata::from_statx(0, 0, -10, 500);
        assert_eq!(
            m.modified(),
            UNIX_EPOCH - Duration::new(10, 0) + Duration::new(0, 500)
        );
        let m = Metadata::from_statx(0, 0, i64::MIN, 0);
        assert!(m.modified() < UNIX_EPOCH);
    }
}
