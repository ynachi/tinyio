#![forbid(unsafe_code)]

use std::io;
use std::net::SocketAddr;
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};

use super::TcpStream;
use super::sockopt;
use crate::cx::Cx;

/// A listening TCP socket. Accept with [`Cx::accept`](crate::Cx::accept) or
/// [`Cx::accept_addr`](crate::Cx::accept_addr); it is an [`AsFd`] for both.
#[derive(Debug)]
pub struct TcpListener {
    fd: OwnedFd,
}

impl TcpListener {
    /// Bind and listen with a backlog of 4096. Sets `SO_REUSEADDR` (as std does)
    /// and `TCP_NODELAY` (accepted sockets inherit it). Like std, it fails with
    /// `AddrInUse` if another socket is listening on `addr`; to let several
    /// listeners share a port use [`bind_reuse_port`](Self::bind_reuse_port).
    pub fn bind(addr: SocketAddr) -> io::Result<TcpListener> {
        TcpListener::bind_with_backlog(addr, 4096)
    }

    /// [`bind`](Self::bind) with the listen backlog given.
    pub fn bind_with_backlog(addr: SocketAddr, backlog: i32) -> io::Result<TcpListener> {
        Ok(TcpListener {
            fd: sockopt::listener(&addr, backlog, false)?,
        })
    }

    /// [`bind`](Self::bind) with `SO_REUSEPORT`, so one `Io` per thread can
    /// listen on the same port and the kernel spreads connections between them.
    /// Every socket sharing the port must set it, and the kernel only admits
    /// sockets of the same user, so a stranger cannot join.
    pub fn bind_reuse_port(addr: SocketAddr) -> io::Result<TcpListener> {
        Ok(TcpListener {
            fd: sockopt::listener(&addr, 4096, true)?,
        })
    }

    /// Wait for a connection and return it as a [`TcpStream`]
    /// ([`Cx::accept`](crate::Cx::accept) with the wrapping done).
    pub fn accept(&self, cx: &Cx) -> io::Result<TcpStream> {
        cx.accept(self).map(TcpStream::from)
    }

    /// The bound address (useful after binding port 0).
    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        sockopt::local_addr(&self.fd)
    }
}

impl AsFd for TcpListener {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.fd.as_fd()
    }
}
