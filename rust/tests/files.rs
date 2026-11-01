#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! File operations for storage code: vectored I/O, durability (`sync_*`),
//! `set_len`, `rename`, removal and `metadata`. Each test states its
//! prediction first. Needs the kernel floor (Linux 6.9).

mod common;

use std::cell::RefCell;
use std::io::{ErrorKind, IoSlice, IoSliceMut};
use std::rc::Rc;
use std::thread;
use std::time::{Duration, SystemTime};

use common::{TmpDir, watchdog};
use tinyio::{Cx, Io, OpenOptions, Options, is_cancelled};

const MS: Duration = Duration::from_millis(1);
const LONG: Duration = Duration::from_secs(10);

fn io() -> Io {
    watchdog(60);
    Io::new(Options::default()).unwrap()
}

fn rw_create() -> OpenOptions {
    let mut o = OpenOptions::new();
    o.read(true).write(true).create(true).truncate(true);
    o
}

fn read_only() -> OpenOptions {
    let mut o = OpenOptions::new();
    o.read(true);
    o
}

/// Run `body` as the only fiber of a fresh `Io`.
fn run(body: impl FnOnce(&Cx) + 'static) {
    let mut io = io();
    io.spawn(body).unwrap();
    io.run_until_done();
}

// Prediction: three buffers written at an offset land back to back there;
// after sync_data and a reopen, three differently sized buffers read them back
// in order, and the counts are the full totals both ways.
#[test]
fn write_vectored_sync_data_reopen_read_vectored() {
    let dir = TmpDir::new("vectored");
    let path = dir.join("f");
    let p = path.clone();
    run(move |cx| {
        let fd = cx.open(&p, &rw_create()).unwrap();
        assert_eq!(cx.write(&fd, b"0123456789", Some(0)).unwrap(), 10);
        let bufs = [
            IoSlice::new(b"abcd"),
            IoSlice::new(b"EFGHIJ"),
            IoSlice::new(b"klmno"),
        ];
        assert_eq!(cx.write_vectored(&fd, &bufs, Some(10)).unwrap(), 15);
        cx.sync_data(&fd).unwrap();
        cx.close(fd).unwrap();

        let fd = cx.open(&p, &read_only()).unwrap();
        let (mut a, mut b, mut c) = ([0u8; 3], [0u8; 5], [0u8; 7]);
        let mut bufs = [
            IoSliceMut::new(&mut a),
            IoSliceMut::new(&mut b),
            IoSliceMut::new(&mut c),
        ];
        assert_eq!(cx.read_vectored(&fd, &mut bufs, Some(10)).unwrap(), 15);
        assert_eq!((&a, &b, &c), (b"abc", b"dEFGH", b"IJklmno"));
        cx.close(fd).unwrap();
    });
    assert_eq!(std::fs::read(&path).unwrap(), b"0123456789abcdEFGHIJklmno");
}

// Prediction: a vectored read that runs into the end of the file returns the
// partial count (the buffers fill in order), and at or past the end it is Ok(0).
#[test]
fn short_vectored_read_then_eof() {
    let dir = TmpDir::new("vshort");
    let path = dir.join("f");
    std::fs::write(&path, b"0123456789").unwrap();
    run(move |cx| {
        let fd = cx.open(&path, &read_only()).unwrap();
        let (mut a, mut b, mut c) = ([0u8; 4], [0u8; 4], [0u8; 4]);
        let mut bufs = [
            IoSliceMut::new(&mut a),
            IoSliceMut::new(&mut b),
            IoSliceMut::new(&mut c),
        ];
        assert_eq!(cx.read_vectored(&fd, &mut bufs, Some(0)).unwrap(), 10);
        assert_eq!((&a, &b, &c[..2]), (b"0123", b"4567", &b"89"[..]));
        let mut bufs = [IoSliceMut::new(&mut a), IoSliceMut::new(&mut b)];
        assert_eq!(cx.read_vectored(&fd, &mut bufs, Some(10)).unwrap(), 0);
        assert_eq!(cx.read_vectored(&fd, &mut bufs, Some(5000)).unwrap(), 0);
        cx.close(fd).unwrap();
    });
}

// Prediction: with no offset, vectored I/O uses and advances the file position,
// like read and write do.
#[test]
fn vectored_io_without_an_offset_uses_the_file_position() {
    let dir = TmpDir::new("vpos");
    let path = dir.join("f");
    run(move |cx| {
        let fd = cx.open(&path, &rw_create()).unwrap();
        let bufs = [IoSlice::new(b"ab"), IoSlice::new(b"cd")];
        assert_eq!(cx.write_vectored(&fd, &bufs, None).unwrap(), 4);
        assert_eq!(cx.write_vectored(&fd, &bufs, None).unwrap(), 4);
        let (mut a, mut b) = ([0u8; 4], [0u8; 4]);
        let mut bufs = [IoSliceMut::new(&mut a), IoSliceMut::new(&mut b)];
        assert_eq!(cx.read_vectored(&fd, &mut bufs, Some(0)).unwrap(), 8);
        assert_eq!((&a, &b), (b"abcd", b"abcd"));
        cx.close(fd).unwrap();
    });
}

// Prediction: the atomic-replace sequence leaves exactly the new content under
// the live name: the old content is gone, the temporary name is gone, and the
// directory can itself be synced.
#[test]
fn atomic_replace_write_sync_rename() {
    let dir = TmpDir::new("replace");
    let live = dir.join("live");
    let tmp = dir.join("live.tmp");
    std::fs::write(&live, b"old content").unwrap();
    let (l, t, d) = (live.clone(), tmp.clone(), dir.path().to_path_buf());
    run(move |cx| {
        let fd = cx.open(&t, &rw_create()).unwrap();
        cx.write(&fd, b"new content", Some(0)).unwrap();
        cx.sync_all(&fd).unwrap();
        cx.close(fd).unwrap();
        cx.rename(&t, &l).unwrap();
        let dfd = cx.open(&d, &read_only()).unwrap();
        cx.sync_all(&dfd).unwrap();
        cx.close(dfd).unwrap();
    });
    assert_eq!(std::fs::read(&live).unwrap(), b"new content");
    assert!(!tmp.exists());
}

// Prediction: renaming a missing file is NotFound and touches nothing.
#[test]
fn rename_of_a_missing_file_is_not_found() {
    let dir = TmpDir::new("rename-missing");
    let (a, b) = (dir.join("nope"), dir.join("to"));
    run(move |cx| {
        assert_eq!(cx.rename(&a, &b).unwrap_err().kind(), ErrorKind::NotFound);
    });
    assert!(!dir.join("to").exists());
}

// Prediction: removing a missing file is NotFound; removing a file works; a
// directory needs remove_dir (remove_file says IsADirectory); a non-empty
// directory is DirectoryNotEmpty; an empty one goes.
#[test]
fn removal_errors_and_success() {
    let dir = TmpDir::new("remove");
    let file = dir.join("f");
    let sub = dir.join("d");
    std::fs::write(&file, b"x").unwrap();
    std::fs::create_dir(&sub).unwrap();
    std::fs::write(sub.join("inner"), b"y").unwrap();
    let (f, s) = (file.clone(), sub.clone());
    run(move |cx| {
        assert_eq!(
            cx.remove_file(dir_missing(&f)).unwrap_err().kind(),
            ErrorKind::NotFound
        );
        cx.remove_file(&f).unwrap();
        assert_eq!(
            cx.remove_file(&s).unwrap_err().kind(),
            ErrorKind::IsADirectory
        );
        assert_eq!(
            cx.remove_dir(&s).unwrap_err().kind(),
            ErrorKind::DirectoryNotEmpty
        );
        cx.remove_file(s.join("inner")).unwrap();
        cx.remove_dir(&s).unwrap();
    });
    assert!(!file.exists() && !sub.exists());
}

fn dir_missing(p: &std::path::Path) -> std::path::PathBuf {
    p.with_file_name("does-not-exist")
}

// Prediction: set_len grows (zero-filled) and shrinks, and metadata().len()
// follows every change: after a write, after growing, after shrinking.
#[test]
fn metadata_len_follows_write_and_set_len() {
    let dir = TmpDir::new("meta-len");
    let path = dir.join("f");
    run(move |cx| {
        let fd = cx.open(&path, &rw_create()).unwrap();
        assert_eq!(cx.metadata(&fd).unwrap().len(), 0);
        cx.write(&fd, b"hello", Some(0)).unwrap();
        assert_eq!(cx.metadata(&fd).unwrap().len(), 5);
        cx.set_len(&fd, 4096).unwrap();
        assert_eq!(cx.metadata(&fd).unwrap().len(), 4096);
        let mut buf = [1u8; 8];
        assert_eq!(cx.read(&fd, &mut buf, Some(5)).unwrap(), 8);
        assert_eq!(buf, [0u8; 8], "the grown part reads as zeros");
        cx.set_len(&fd, 2).unwrap();
        assert_eq!(cx.metadata(&fd).unwrap().len(), 2);
        assert_eq!(cx.metadata_path(&path).unwrap().len(), 2);
        cx.close(fd).unwrap();
    });
}

// Prediction: is_file / is_dir tell a file from a directory (by descriptor and
// by path), mode carries the permission bits, modified() is now-ish, and a
// missing path is NotFound.
#[test]
fn metadata_kinds_mode_and_mtime() {
    let dir = TmpDir::new("meta-kind");
    let file = dir.join("f");
    std::fs::write(&file, b"x").unwrap();
    let d = dir.path().to_path_buf();
    run(move |cx| {
        let m = cx.metadata_path(&file).unwrap();
        assert!(m.is_file() && !m.is_dir());
        assert_eq!(m.len(), 1);
        let age = SystemTime::now()
            .duration_since(m.modified())
            .unwrap_or_default();
        assert!(age < Duration::from_secs(60), "{age:?}");

        let m = cx.metadata_path(&d).unwrap();
        assert!(m.is_dir() && !m.is_file());
        let dfd = cx.open(&d, &read_only()).unwrap();
        assert!(cx.metadata(&dfd).unwrap().is_dir());
        cx.close(dfd).unwrap();

        let mut o = rw_create();
        o.mode(0o640);
        let path = d.join("m");
        let fd = cx.open(&path, &o).unwrap();
        let mode = cx.metadata(&fd).unwrap().mode();
        assert_eq!(mode & 0o7777 & !0o022, 0o640 & !0o022, "{mode:o}");
        assert_eq!(mode & libc_s_ifmt(), libc_s_ifreg());
        cx.close(fd).unwrap();

        assert_eq!(
            cx.metadata_path(d.join("nope")).unwrap_err().kind(),
            ErrorKind::NotFound
        );
    });
}

// S_IFMT and S_IFREG, spelled out so the test needs no libc dependency.
fn libc_s_ifmt() -> u32 {
    0o170000
}
fn libc_s_ifreg() -> u32 {
    0o100000
}

// Prediction: 1,024 buffers (IOV_MAX) work; 1,025 are refused with EINVAL by
// the call itself, not by a crash, for both directions.
#[test]
fn iov_max_plus_one_is_einval() {
    let dir = TmpDir::new("iovmax");
    let path = dir.join("f");
    run(move |cx| {
        let fd = cx.open(&path, &rw_create()).unwrap();
        let one = [7u8];
        let bufs: Vec<IoSlice<'_>> = (0..1024).map(|_| IoSlice::new(&one)).collect();
        assert_eq!(cx.write_vectored(&fd, &bufs, Some(0)).unwrap(), 1024);
        let bufs: Vec<IoSlice<'_>> = (0..1025).map(|_| IoSlice::new(&one)).collect();
        let e = cx.write_vectored(&fd, &bufs, Some(0)).unwrap_err();
        assert_eq!(e.raw_os_error(), Some(22), "{e:?}");

        let mut bytes = vec![0u8; 1025];
        let mut bufs: Vec<IoSliceMut<'_>> = bytes
            .iter_mut()
            .map(std::slice::from_mut)
            .map(IoSliceMut::new)
            .collect();
        let e = cx.read_vectored(&fd, &mut bufs, Some(0)).unwrap_err();
        assert_eq!(e.raw_os_error(), Some(22), "{e:?}");
        bufs.truncate(1024);
        assert_eq!(cx.read_vectored(&fd, &mut bufs, Some(0)).unwrap(), 1024);
        cx.close(fd).unwrap();
    });
}

// Prediction: every timed form behaves like its plain twin when the deadline is
// long, and leaves nothing armed (the fiber finishes without the armed-timer abort).
#[test]
fn timed_forms_complete_normally() {
    let dir = TmpDir::new("timed");
    let (a, b, d) = (dir.join("a"), dir.join("b"), dir.path().to_path_buf());
    run(move |cx| {
        let t = cx.timeout(LONG);
        let fd = cx.open(&a, &rw_create()).unwrap();
        assert_eq!(
            t.write_vectored(&fd, &[IoSlice::new(b"he"), IoSlice::new(b"llo")], Some(0))
                .unwrap(),
            5
        );
        t.sync_data(&fd).unwrap();
        t.sync_all(&fd).unwrap();
        let (mut x, mut y) = ([0u8; 2], [0u8; 3]);
        let mut bufs = [IoSliceMut::new(&mut x), IoSliceMut::new(&mut y)];
        assert_eq!(t.read_vectored(&fd, &mut bufs, Some(0)).unwrap(), 5);
        assert_eq!((&x, &y), (b"he", b"llo"));
        t.set_len(&fd, 3).unwrap();
        assert_eq!(t.metadata(&fd).unwrap().len(), 3);
        assert_eq!(t.metadata_path(&a).unwrap().len(), 3);
        t.rename(&a, &b).unwrap();
        t.remove_file(&b).unwrap();
        std::fs::create_dir(d.join("sub")).unwrap();
        t.remove_dir(d.join("sub")).unwrap();
        cx.close(fd).unwrap();
    });
}

// Prediction: a deadline that passes while a vectored read waits (an empty
// pipe) is TimedOut.
#[test]
fn timed_read_vectored_times_out() {
    let (r, _w) = std::io::pipe().unwrap();
    run(move |cx| {
        let mut buf = [0u8; 4];
        let mut bufs = [IoSliceMut::new(&mut buf)];
        let e = cx
            .timeout(30 * MS)
            .read_vectored(&r, &mut bufs, None)
            .unwrap_err();
        assert_eq!(e.kind(), ErrorKind::TimedOut);
    });
}

// Prediction: a stop while a vectored read is in flight (plain and timed) ends
// it with ECANCELED, and the loop returns: no hang.
#[test]
fn stop_cancels_an_in_flight_vectored_read() {
    let (r, w) = std::io::pipe().unwrap();
    let mut io = io();
    let remote = io.remote();
    let seen = Rc::new(RefCell::new(Vec::new()));
    let (s1, s2) = (seen.clone(), seen.clone());
    let r2 = r.try_clone().unwrap();
    io.spawn(move |cx| {
        let mut buf = [0u8; 4];
        let e = cx
            .read_vectored(&r, &mut [IoSliceMut::new(&mut buf)], None)
            .unwrap_err();
        s1.borrow_mut().push(is_cancelled(&e));
    })
    .unwrap();
    io.spawn(move |cx| {
        let mut buf = [0u8; 4];
        let e = cx
            .timeout(LONG)
            .read_vectored(&r2, &mut [IoSliceMut::new(&mut buf)], None)
            .unwrap_err();
        s2.borrow_mut().push(is_cancelled(&e));
    })
    .unwrap();
    let stopper = thread::spawn(move || {
        thread::sleep(50 * MS);
        remote.stop();
    });
    io.run();
    stopper.join().unwrap();
    drop(w);
    assert_eq!(*seen.borrow(), [true, true]);
}

// Prediction: once the runtime is stopping, every new operation (each of the
// plain forms and each timed form) fails at once with ECANCELED and touches
// nothing: the file is not renamed, removed or resized.
#[test]
fn every_operation_is_refused_while_stopping() {
    let dir = TmpDir::new("stopping");
    let path = dir.join("f");
    std::fs::write(&path, b"keep").unwrap();
    let mut io = io();
    // A listener fd to hold: the fiber starts only once the stop is in force.
    let remote = io.remote();
    let p = path.clone();
    let got = Rc::new(RefCell::new(Vec::new()));
    let g = got.clone();
    io.spawn(move |cx| {
        let file = std::fs::File::open(&p).unwrap();
        let mut buf = [0u8; 2];
        let t = cx.timeout(LONG);
        let push = |name: &'static str, e: std::io::Error| {
            g.borrow_mut().push((name, is_cancelled(&e)));
        };
        push("sync_all", cx.sync_all(&file).unwrap_err());
        push("sync_data", cx.sync_data(&file).unwrap_err());
        push(
            "read_vectored",
            cx.read_vectored(&file, &mut [IoSliceMut::new(&mut buf)], Some(0))
                .unwrap_err(),
        );
        push(
            "write_vectored",
            cx.write_vectored(&file, &[IoSlice::new(b"x")], Some(0))
                .unwrap_err(),
        );
        push("set_len", cx.set_len(&file, 1).unwrap_err());
        push(
            "rename",
            cx.rename(&p, p.with_extension("moved")).unwrap_err(),
        );
        push("remove_file", cx.remove_file(&p).unwrap_err());
        push("remove_dir", cx.remove_dir(&p).unwrap_err());
        push("metadata", cx.metadata(&file).unwrap_err());
        push("metadata_path", cx.metadata_path(&p).unwrap_err());
        push("timed sync_all", t.sync_all(&file).unwrap_err());
        push("timed sync_data", t.sync_data(&file).unwrap_err());
        push(
            "timed read_vectored",
            t.read_vectored(&file, &mut [IoSliceMut::new(&mut buf)], Some(0))
                .unwrap_err(),
        );
        push(
            "timed write_vectored",
            t.write_vectored(&file, &[IoSlice::new(b"x")], Some(0))
                .unwrap_err(),
        );
        push("timed set_len", t.set_len(&file, 1).unwrap_err());
        push(
            "timed rename",
            t.rename(&p, p.with_extension("moved")).unwrap_err(),
        );
        push("timed remove_file", t.remove_file(&p).unwrap_err());
        push("timed remove_dir", t.remove_dir(&p).unwrap_err());
        push("timed metadata", t.metadata(&file).unwrap_err());
        push("timed metadata_path", t.metadata_path(&p).unwrap_err());
    })
    .unwrap();
    remote.stop(); // before the loop starts: the fiber's first act runs under the stop
    io.run();
    let got = got.borrow();
    assert_eq!(got.len(), 20);
    for (name, cancelled) in got.iter() {
        assert!(cancelled, "{name} was not refused with ECANCELED");
    }
    assert_eq!(std::fs::read(&path).unwrap(), b"keep");
    assert!(!path.with_extension("moved").exists());
}
