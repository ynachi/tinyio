#![forbid(unsafe_code)]

use std::io;
use std::net::SocketAddr;
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};

use super::sockopt;

/// A listening TCP socket. Accept with [`Cx::accept`](crate::Cx::accept) or
/// [`Cx::accept_addr`](crate::Cx::accept_addr); it is an [`AsFd`] for both.
#[derive(Debug)]
pub struct TcpListener {
    fd: OwnedFd,
}

impl TcpListener {
    /// Bind and listen with a backlog of 4096. Sets `SO_REUSEADDR`,
    /// `SO_REUSEPORT` (so one `Io` per thread can bind the same port) and
    /// `TCP_NODELAY` (accepted sockets inherit it).
    pub fn bind(addr: SocketAddr) -> io::Result<TcpListener> {
        TcpListener::bind_with_backlog(addr, 4096)
    }

    pub fn bind_with_backlog(addr: SocketAddr, backlog: i32) -> io::Result<TcpListener> {
        Ok(TcpListener {
            fd: sockopt::listener(&addr, backlog)?,
        })
    }

    /// The bound address (useful after binding port 0).
    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        // A duplicate of the descriptor, only so std can ask the kernel.
        std::net::TcpListener::from(self.fd.try_clone()?).local_addr()
    }
}

impl AsFd for TcpListener {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.fd.as_fd()
    }
}
