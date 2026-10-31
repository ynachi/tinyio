//! `Cx`: what a fiber holds. It carries the scheduler handle and nothing else
//! (Rule 13: a fiber's identity is `sched.current`, never the `Cx` it was
//! given, so a scoped child may legally hold its parent's `&Cx`).
//!
//! Every I/O call goes through [`Cx::syscall`], the one path an SQE takes
//! from a fiber. `unsafe` is confined to that path and the thin typed wrappers
//! over it, each of which borrows its buffer for the whole blocking call.

use std::ffi::CString;
use std::io;
use std::marker::PhantomData;
use std::net::SocketAddr;
use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::rc::Rc;
use std::time::Duration;

use io_uring::opcode;
use io_uring::squeue::Entry;
use io_uring::types::{Fd, Timespec};

use crate::check::check;
use crate::detail::fiber::{FiberId, FiberState, WaitKind};
use crate::fs::OpenOptions;
use crate::net::PollEvents;
use crate::net::addr::{self, RawAddr};
use crate::ring::{Completion, UserData};
use crate::sched::Sched;

/// Marker that makes a type `!Send + !Sync` (Rule 1).
pub(crate) type NotSend = PhantomData<*const ()>;

/// The kernel's cap on one read or write; longer buffers get a short count.
const MAX_RW_LEN: usize = 0x7fff_f000;

fn errno(code: i32) -> io::Error {
    io::Error::from_raw_os_error(code)
}

/// Why a `syscall` failed, and whether the SQE ever reached the ring. A
/// descriptor handed to a refused `close` is still ours; one handed to a close
/// that ran is gone.
struct Failed {
    err: io::Error,
    submitted: bool,
}

/// The handle a fiber uses to reach its runtime: spawn, yield and perform
/// I/O. `!Send`: it never leaves the thread of its `Io`.
pub struct Cx {
    sched: Rc<Sched>,
    _p: NotSend,
}

impl Cx {
    pub(crate) fn new(sched: Rc<Sched>) -> Cx {
        Cx {
            sched,
            _p: PhantomData,
        }
    }

    #[inline]
    pub(crate) fn sched(&self) -> &Rc<Sched> {
        &self.sched
    }

    /// Start another fiber on this thread. It runs after the current one
    /// next suspends. `f` must own everything it uses (`'static`); it cannot
    /// capture this `Cx`, and receives its own. `ECANCELED` once the runtime
    /// is shutting down.
    pub fn spawn<F>(&self, f: F) -> io::Result<FiberId>
    where
        F: FnOnce(&Cx) + 'static,
    {
        self.sched.spawn(f)
    }

    /// Give the thread to other ready fibers; resume after they have had a turn.
    /// `ECANCELED` once the runtime is shutting down (invariant 11), so a
    /// loop that checks the result ends.
    pub fn yield_now(&self) -> io::Result<()> {
        if self.sched.stopping() {
            return Err(errno(libc::ECANCELED));
        }
        self.sched.yield_current();
        Ok(())
    }

    /// The one path every I/O call takes (invariants 1, 2, 3, 11):
    ///
    /// 1. refuse with `ECANCELED` if the runtime is stopping, touching nothing;
    /// 2. queue the SQE (if the SQ is full, submit and try once more);
    /// 3. park on `WaitKind::Io`: the `Completion` below stays on this stack
    ///    until the CQE arrives, and nothing else can resume the fiber;
    /// 4. map a negative result to an `io::Error`.
    ///
    /// `Failed::submitted` says whether step 2 succeeded.
    ///
    /// # Safety
    /// Every pointer inside `entry` must stay valid until this call returns.
    /// Callers pass pointers into data they borrow across the call: the fiber
    /// is parked until the CQE, so the borrow outlives the kernel's use.
    unsafe fn syscall_ex(&self, entry: &Entry, timeout: Option<Duration>) -> Result<u32, Failed> {
        let not_submitted = |err| Failed {
            err,
            submitted: false,
        };
        let sched = &*self.sched;
        if sched.stopping() {
            return Err(not_submitted(errno(libc::ECANCELED)));
        }
        let me = sched.current();
        // SAFETY: the running fiber's header is live.
        let state = unsafe { me.as_ref() }.state.get();
        check!(
            state == FiberState::Running,
            "syscall from a non-running fiber (invariant 1)"
        );

        let completion = Completion::new(me);
        let ud = UserData::Fiber(std::ptr::NonNull::from(&completion));
        // SAFETY: pointer validity is this fn's contract; `completion` lives
        // in this frame, which does not return before the CQE (park below).
        unsafe { sched.push_sqe(entry, ud) }.map_err(not_submitted)?;

        // SAFETY: the running fiber's header is live.
        let header = unsafe { me.as_ref() };
        header.timed_out.set(false);
        header.op_user_data.set(ud.encode());
        if let Some(d) = timeout {
            sched.arm_timeout(me, d);
        }

        sched.park(WaitKind::Io);

        let res = completion.result();
        if res == -libc::ECANCELED && header.timed_out.get() {
            // The wheel expired the op and cancelled it: that is a timeout.
            Err(Failed {
                err: errno(libc::ETIMEDOUT),
                submitted: true,
            })
        } else if res < 0 {
            Err(Failed {
                err: errno(-res),
                submitted: true,
            })
        } else {
            Ok(res.unsigned_abs())
        }
    }

    /// [`syscall_ex`](Self::syscall_ex) for callers that only want the error.
    ///
    /// # Safety
    /// As `syscall_ex`.
    unsafe fn syscall(&self, entry: &Entry, timeout: Option<Duration>) -> io::Result<u32> {
        // SAFETY: forwarded.
        unsafe { self.syscall_ex(entry, timeout) }.map_err(|f| f.err)
    }

    /// The same I/O calls, each failing with `ETIMEDOUT` after `d`.
    pub fn timeout(&self, d: Duration) -> Timed<'_> {
        Timed { cx: self, d }
    }

    /// `read(2)` at `offset`, or at the file position when `None`. May return
    /// fewer bytes than `buf.len()` (always at most 0x7fff_f000). `Ok(0)` is
    /// end of file.
    pub fn read(&self, fd: &impl AsFd, buf: &mut [u8], offset: Option<u64>) -> io::Result<usize> {
        self.read_with(fd, buf, offset, None)
    }

    fn read_with(
        &self,
        fd: &impl AsFd,
        buf: &mut [u8],
        offset: Option<u64>,
        timeout: Option<Duration>,
    ) -> io::Result<usize> {
        let off = file_offset(offset)?;
        let len = buf.len().min(MAX_RW_LEN) as u32;
        let entry = opcode::Read::new(Fd(fd.as_fd().as_raw_fd()), buf.as_mut_ptr(), len)
            .offset(off)
            .build();
        // SAFETY: `buf` is borrowed for this whole call and the fiber is
        // parked until the CQE, so the kernel's writes land in live memory;
        // `fd` is borrowed too, so it stays open.
        unsafe { self.syscall(&entry, timeout) }.map(|n| n as usize)
    }

    /// `write(2)` at `offset`, or at the file position when `None`. May write
    /// fewer bytes than `buf.len()`.
    pub fn write(&self, fd: &impl AsFd, buf: &[u8], offset: Option<u64>) -> io::Result<usize> {
        self.write_with(fd, buf, offset, None)
    }

    fn write_with(
        &self,
        fd: &impl AsFd,
        buf: &[u8],
        offset: Option<u64>,
        timeout: Option<Duration>,
    ) -> io::Result<usize> {
        let off = file_offset(offset)?;
        let len = buf.len().min(MAX_RW_LEN) as u32;
        let entry = opcode::Write::new(Fd(fd.as_fd().as_raw_fd()), buf.as_ptr(), len)
            .offset(off)
            .build();
        // SAFETY: as in `read`: `buf` and `fd` are borrowed across the call.
        unsafe { self.syscall(&entry, timeout) }.map(|n| n as usize)
    }

    /// Accept a connection, parking the fiber until one arrives. The new
    /// descriptor is close-on-exec; wrap it with `TcpStream::from`.
    ///
    /// The listener is borrowed while the call is parked, so it cannot be closed
    /// under it:
    ///
    /// ```compile_fail,E0505
    /// use tinyio::net::TcpListener;
    /// let mut io = tinyio::Io::new(Default::default()).unwrap();
    /// io.spawn(|cx| {
    ///     let l = TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap();
    ///     tinyio::scope(cx, |s| {
    ///         s.spawn(|c| { let _ = c.accept(&l); }).unwrap();
    ///         drop(l); // closes the fd the child is parked on
    ///     });
    /// }).unwrap();
    /// io.run_until_done();
    /// ```
    /// ```
    /// use tinyio::net::TcpListener;
    /// let mut io = tinyio::Io::new(Default::default()).unwrap();
    /// io.spawn(|cx| {
    ///     let l = TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap();
    ///     tinyio::scope(cx, |s| {
    ///         s.spawn(|c| { let _ = c.timeout(std::time::Duration::from_millis(1)).accept(&l); }).unwrap();
    ///     });
    ///     drop(l); // the twin: dropped after the scope has joined
    /// }).unwrap();
    /// io.run_until_done();
    /// ```
    pub fn accept(&self, listener: &impl AsFd) -> io::Result<OwnedFd> {
        self.accept_with(listener, None).map(|(fd, _)| fd)
    }

    /// [`accept`](Self::accept), also returning the peer's address.
    pub fn accept_addr(&self, listener: &impl AsFd) -> io::Result<(OwnedFd, SocketAddr)> {
        let (fd, peer) = self.accept_with(listener, None)?;
        peer.map(|peer| (fd, peer))
            .ok_or_else(|| errno(libc::EAFNOSUPPORT))
    }

    fn accept_with(
        &self,
        listener: &impl AsFd,
        timeout: Option<Duration>,
    ) -> io::Result<(OwnedFd, Option<SocketAddr>)> {
        let mut raw = RawAddr::zeroed();
        let mut len = size_of::<RawAddr>() as libc::socklen_t;
        let entry = opcode::Accept::new(
            Fd(listener.as_fd().as_raw_fd()),
            raw.bytes.as_mut_ptr().cast(),
            &raw mut len,
        )
        .flags(libc::SOCK_CLOEXEC)
        .build();
        // SAFETY: `raw` and `len` are locals of this frame, alive across the
        // call; `listener` is borrowed, so it stays open.
        let fd = unsafe { self.syscall(&entry, timeout) }?;
        // SAFETY: a successful accept returns a fresh descriptor that nothing
        // else owns.
        let fd = unsafe { OwnedFd::from_raw_fd(fd as RawFd) };
        Ok((fd, addr::decode(&raw, len)))
    }

    /// Connect a socket to `addr`, parking the fiber until the handshake is
    /// done. See `TcpStream::connect` for the usual way in.
    pub fn connect(&self, socket: &impl AsFd, addr: &SocketAddr) -> io::Result<()> {
        self.connect_with(socket, addr, None)
    }

    fn connect_with(
        &self,
        socket: &impl AsFd,
        addr: &SocketAddr,
        timeout: Option<Duration>,
    ) -> io::Result<()> {
        let (raw, len) = addr::encode(addr);
        let entry = opcode::Connect::new(
            Fd(socket.as_fd().as_raw_fd()),
            raw.bytes.as_ptr().cast(),
            len,
        )
        .build();
        // SAFETY: `raw` is a local of this frame, alive across the call;
        // `socket` is borrowed, so it stays open.
        unsafe { self.syscall(&entry, timeout) }.map(drop)
    }

    /// Wait until `fd` is ready for any of `events`; returns the events that
    /// are ready (`ERR` and `HUP` can be set without being asked for).
    pub fn poll(&self, fd: &impl AsFd, events: PollEvents) -> io::Result<PollEvents> {
        self.poll_with(fd, events, None)
    }

    fn poll_with(
        &self,
        fd: &impl AsFd,
        events: PollEvents,
        timeout: Option<Duration>,
    ) -> io::Result<PollEvents> {
        let entry = opcode::PollAdd::new(Fd(fd.as_fd().as_raw_fd()), events.bits()).build();
        // SAFETY: the entry carries no pointers; `fd` is borrowed across the call.
        unsafe { self.syscall(&entry, timeout) }.map(PollEvents::from_bits)
    }

    /// `openat(2)` relative to the current directory.
    pub fn open(&self, path: impl AsRef<Path>, opts: &OpenOptions) -> io::Result<OwnedFd> {
        let flags = opts.flags()?;
        let c_path = CString::new(path.as_ref().as_os_str().as_bytes())
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "path contains a NUL byte"))?;
        let entry = opcode::OpenAt::new(Fd(libc::AT_FDCWD), c_path.as_ptr())
            .flags(flags)
            .mode(opts.mode_bits())
            .build();
        // SAFETY: `c_path` is a local of this frame, alive across the call.
        let fd = unsafe { self.syscall(&entry, None) }?;
        // SAFETY: a successful openat returns a fresh descriptor that nothing
        // else in the process owns or will close.
        Ok(unsafe { OwnedFd::from_raw_fd(fd as RawFd) })
    }

    /// `close(2)`, asynchronously. Takes the descriptor. It is consumed
    /// whenever the close was submitted and ran (even if the kernel reported an
    /// error: the descriptor is gone either way). If the call never ran (refused
    /// before reaching the ring, or cancelled before it executed) the descriptor
    /// is closed synchronously by dropping it, and the error says why.
    pub fn close(&self, fd: OwnedFd) -> io::Result<()> {
        let entry = opcode::Close::new(Fd(fd.as_raw_fd())).build();
        // SAFETY: the entry carries no pointers.
        match unsafe { self.syscall_ex(&entry, None) } {
            Ok(_) => {
                // The kernel closed it; dropping `fd` would close it again.
                std::mem::forget(fd);
                Ok(())
            }
            Err(Failed { err, submitted }) => {
                let never_ran = !submitted || err.raw_os_error() == Some(libc::ECANCELED);
                if never_ran {
                    drop(fd); // still ours: close it here
                } else {
                    std::mem::forget(fd); // failed in the kernel, which dropped it
                }
                Err(err)
            }
        }
    }

    /// Sleep for `d` with an io_uring timeout SQE. It counts in flight, so
    /// the loop sees a sleeping fiber as waiting on a completion, not as
    /// stuck. `Ok` when the time has passed; `ECANCELED` if shutdown cut it
    /// short. A zero duration returns at once without touching the ring.
    pub fn sleep(&self, d: Duration) -> io::Result<()> {
        if d.is_zero() {
            return Ok(());
        }
        let ts = Timespec::new()
            .sec(d.as_secs().min(i64::MAX as u64))
            .nsec(d.subsec_nanos());
        let entry = opcode::Timeout::new(&raw const ts).build();
        // SAFETY: `ts` is a local of this frame, alive across the call.
        match unsafe { self.syscall(&entry, None) } {
            // A timeout SQE completes with -ETIME when it fires: success here.
            Err(e) if e.raw_os_error() == Some(libc::ETIME) => Ok(()),
            r => r.map(|_| ()),
        }
    }
}

/// A [`Cx`] whose next I/O call gives up after a deadline, from [`Cx::timeout`].
/// The call fails with `ETIMEDOUT` (`io::ErrorKind::TimedOut`). If the runtime
/// is shutting down instead, it fails with `ECANCELED`, never `ETIMEDOUT`.
///
/// Deadlines are kept on a coarse wheel (64 ms slots), so a timeout can fire up
/// to one slot late, and a zero duration expires on the next pass of the loop.
pub struct Timed<'a> {
    cx: &'a Cx,
    d: Duration,
}

impl Timed<'_> {
    /// [`Cx::read`] with the deadline.
    pub fn read(&self, fd: &impl AsFd, buf: &mut [u8], offset: Option<u64>) -> io::Result<usize> {
        self.cx.read_with(fd, buf, offset, Some(self.d))
    }

    /// [`Cx::write`] with the deadline.
    pub fn write(&self, fd: &impl AsFd, buf: &[u8], offset: Option<u64>) -> io::Result<usize> {
        self.cx.write_with(fd, buf, offset, Some(self.d))
    }

    /// [`Cx::accept`] with the deadline.
    pub fn accept(&self, listener: &impl AsFd) -> io::Result<OwnedFd> {
        self.cx
            .accept_with(listener, Some(self.d))
            .map(|(fd, _)| fd)
    }

    /// [`Cx::accept_addr`] with the deadline.
    pub fn accept_addr(&self, listener: &impl AsFd) -> io::Result<(OwnedFd, SocketAddr)> {
        let (fd, peer) = self.cx.accept_with(listener, Some(self.d))?;
        peer.map(|peer| (fd, peer))
            .ok_or_else(|| errno(libc::EAFNOSUPPORT))
    }

    /// [`Cx::connect`] with the deadline.
    pub fn connect(&self, socket: &impl AsFd, addr: &SocketAddr) -> io::Result<()> {
        self.cx.connect_with(socket, addr, Some(self.d))
    }

    /// [`Cx::poll`] with the deadline.
    pub fn poll(&self, fd: &impl AsFd, events: PollEvents) -> io::Result<PollEvents> {
        self.cx.poll_with(fd, events, Some(self.d))
    }
}

/// `None` is the file position (-1); an offset above `i64::MAX` is invalid.
fn file_offset(offset: Option<u64>) -> io::Result<u64> {
    match offset {
        None => Ok(u64::MAX),
        Some(o) if o <= i64::MAX as u64 => Ok(o),
        Some(_) => Err(errno(libc::EINVAL)),
    }
}

/// `true` if `e` is the error a call returns when the runtime is shutting
/// down (`ECANCELED`). It has no `io::ErrorKind`, so match the raw code.
pub fn is_cancelled(e: &io::Error) -> bool {
    e.raw_os_error() == Some(libc::ECANCELED)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn offsets() {
        assert_eq!(file_offset(None).unwrap(), u64::MAX);
        assert_eq!(file_offset(Some(0)).unwrap(), 0);
        assert_eq!(file_offset(Some(i64::MAX as u64)).unwrap(), i64::MAX as u64);
        assert_eq!(
            file_offset(Some(1 << 63)).unwrap_err().raw_os_error(),
            Some(libc::EINVAL)
        );
    }

    #[test]
    fn cancelled_matches_only_ecanceled() {
        assert!(is_cancelled(&errno(libc::ECANCELED)));
        assert!(!is_cancelled(&errno(libc::EBADF)));
    }
}
