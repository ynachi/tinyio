#![forbid(unsafe_code)]

use std::io;
use std::net::{Shutdown, SocketAddr};
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
use std::time::Duration;

use super::sockopt;
use crate::cx::Cx;

/// A connected TCP socket. Read and write it with [`Cx::read`] and
/// [`Cx::send`] (it is an [`AsFd`]), or through [`TcpStream::with`]. Prefer
/// `send` to `write`: it never raises `SIGPIPE`.
#[derive(Debug)]
pub struct TcpStream {
    fd: OwnedFd,
}

impl TcpStream {
    /// Connect to `addr`, parking the fiber until the handshake finishes.
    pub fn connect(cx: &Cx, addr: SocketAddr) -> io::Result<TcpStream> {
        let fd = sockopt::tcp_socket(&addr)?;
        cx.connect(&fd, &addr)?;
        Ok(TcpStream { fd })
    }

    /// Like [`connect`](Self::connect), failing with `TimedOut` after `d`.
    pub fn connect_timeout(cx: &Cx, addr: SocketAddr, d: Duration) -> io::Result<TcpStream> {
        let fd = sockopt::tcp_socket(&addr)?;
        cx.timeout(d).connect(&fd, &addr)?;
        Ok(TcpStream { fd })
    }

    pub fn set_nodelay(&self, on: bool) -> io::Result<()> {
        sockopt::set_nodelay(&self.fd, on)
    }

    pub fn set_keepalive(&self, on: bool) -> io::Result<()> {
        sockopt::set_keepalive(&self.fd, on)
    }

    pub fn peer_addr(&self) -> io::Result<SocketAddr> {
        sockopt::peer_addr(&self.fd)
    }

    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        sockopt::local_addr(&self.fd)
    }

    /// Shut down one or both directions. Blocking, but it never waits.
    pub fn shutdown(&self, how: Shutdown) -> io::Result<()> {
        sockopt::shutdown(&self.fd, how)
    }

    /// Bind this stream to `cx` so `std::io::Read` and `Write` work on it
    /// (`read_exact`, `write_all`, `BufReader`, `io::copy`, ...). The traits
    /// have nowhere to take a `Cx`, hence the adapter. Each call parks the fiber.
    pub fn with<'a>(&'a self, cx: &'a Cx) -> Bound<'a> {
        Bound { stream: self, cx }
    }
}

impl From<TcpStream> for OwnedFd {
    /// Take the descriptor back, e.g. to close it through the ring with
    /// [`Cx::close`] (dropping a `TcpStream` closes it with a blocking `close(2)`).
    fn from(stream: TcpStream) -> OwnedFd {
        stream.fd
    }
}

impl From<OwnedFd> for TcpStream {
    /// Wrap an accepted descriptor (what [`Cx::accept`] returns).
    fn from(fd: OwnedFd) -> TcpStream {
        TcpStream { fd }
    }
}

impl AsFd for TcpStream {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.fd.as_fd()
    }
}

/// A [`TcpStream`] together with the [`Cx`] to do its I/O on, from
/// [`TcpStream::with`]. It implements `std::io::Read` and `Write`.
pub struct Bound<'a> {
    stream: &'a TcpStream,
    cx: &'a Cx,
}

impl std::fmt::Debug for Bound<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Bound")
            .field("stream", &self.stream)
            .finish_non_exhaustive()
    }
}

impl io::Read for Bound<'_> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.cx.read(self.stream, buf, None)
    }

    fn read_vectored(&mut self, bufs: &mut [io::IoSliceMut<'_>]) -> io::Result<usize> {
        self.cx.read_vectored(self.stream, bufs, None)
    }
}

impl io::Write for Bound<'_> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.cx.send(self.stream, buf)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
