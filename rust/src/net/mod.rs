//! TCP on top of `Cx`: a listener, a stream, and the readiness bits `poll` uses.

pub(crate) mod addr;
mod listener;
mod sockopt;
mod stream;

use std::ops::BitOr;

pub use listener::TcpListener;
pub use stream::{Bound, TcpStream};

/// Readiness bits for [`Cx::poll`](crate::Cx::poll), as in `poll(2)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PollEvents(u32);

impl PollEvents {
    pub const IN: PollEvents = PollEvents(libc::POLLIN as u32);
    pub const OUT: PollEvents = PollEvents(libc::POLLOUT as u32);
    pub const ERR: PollEvents = PollEvents(libc::POLLERR as u32);
    pub const HUP: PollEvents = PollEvents(libc::POLLHUP as u32);

    pub(crate) fn from_bits(bits: u32) -> PollEvents {
        PollEvents(bits)
    }

    pub(crate) fn bits(self) -> u32 {
        self.0
    }

    /// Every bit of `other` is set.
    pub fn contains(self, other: PollEvents) -> bool {
        self.0 & other.0 == other.0
    }
}

impl BitOr for PollEvents {
    type Output = PollEvents;

    fn bitor(self, rhs: PollEvents) -> PollEvents {
        PollEvents(self.0 | rhs.0)
    }
}
