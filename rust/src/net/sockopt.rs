//! The thin libc layer: socket creation and the options std cannot set
//! (`SO_REUSEPORT`, the listen backlog). Every function is safe to call.

use std::io;
use std::net::SocketAddr;
use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd};

use super::addr::encode;

fn check(rc: libc::c_int) -> io::Result<libc::c_int> {
    if rc < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(rc)
    }
}

fn family(addr: &SocketAddr) -> libc::c_int {
    match addr {
        SocketAddr::V4(_) => libc::AF_INET,
        SocketAddr::V6(_) => libc::AF_INET6,
    }
}

/// A new close-on-exec TCP socket of `addr`'s family.
pub(crate) fn tcp_socket(addr: &SocketAddr) -> io::Result<OwnedFd> {
    // SAFETY: socket takes no pointers; the result is checked.
    let fd =
        check(unsafe { libc::socket(family(addr), libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0) })?;
    // SAFETY: socket returned a fresh descriptor that nothing else owns.
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

fn set_int(fd: &impl AsFd, level: libc::c_int, name: libc::c_int, value: bool) -> io::Result<()> {
    let value = libc::c_int::from(value);
    // SAFETY: the pointer and length describe `value`, a live c_int; the fd is
    // borrowed for the call.
    check(unsafe {
        libc::setsockopt(
            fd.as_fd().as_raw_fd(),
            level,
            name,
            (&raw const value).cast(),
            size_of::<libc::c_int>() as libc::socklen_t,
        )
    })
    .map(drop)
}

pub(crate) fn set_nodelay(fd: &impl AsFd, on: bool) -> io::Result<()> {
    set_int(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY, on)
}

pub(crate) fn set_keepalive(fd: &impl AsFd, on: bool) -> io::Result<()> {
    set_int(fd, libc::SOL_SOCKET, libc::SO_KEEPALIVE, on)
}

fn set_nonblocking(fd: &impl AsFd) -> io::Result<()> {
    let raw = fd.as_fd().as_raw_fd();
    // SAFETY: fcntl with integer arguments on a descriptor borrowed for the call.
    let flags = check(unsafe { libc::fcntl(raw, libc::F_GETFL) })?;
    // SAFETY: as above.
    check(unsafe { libc::fcntl(raw, libc::F_SETFL, flags | libc::O_NONBLOCK) }).map(drop)
}

/// A listening socket with the C++ defaults: `SO_REUSEADDR`, `SO_REUSEPORT`
/// (so one thread per core can bind the same port), `TCP_NODELAY` (inherited
/// by accepted sockets), non-blocking.
pub(crate) fn listener(addr: &SocketAddr, backlog: i32) -> io::Result<OwnedFd> {
    let fd = tcp_socket(addr)?;
    set_int(&fd, libc::SOL_SOCKET, libc::SO_REUSEADDR, true)?;
    set_int(&fd, libc::SOL_SOCKET, libc::SO_REUSEPORT, true)?;
    set_nodelay(&fd, true)?;
    set_nonblocking(&fd)?;
    let (raw, len) = encode(addr);
    // SAFETY: `raw` holds `len` valid bytes of a sockaddr and outlives the call.
    check(unsafe { libc::bind(fd.as_raw_fd(), raw.bytes.as_ptr().cast(), len) })?;
    // SAFETY: listen takes integers only.
    check(unsafe { libc::listen(fd.as_raw_fd(), backlog) })?;
    Ok(fd)
}
