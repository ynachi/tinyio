//! Fiber synchronization. Every primitive holds an `Rc<Sched>`, so releasing
//! one needs no `Cx`, and is `!Send` (Rule 1).

mod baton;
mod latch;
mod mutex;
mod scope;
mod semaphore;
mod waiters;

use std::rc::Rc;

use crate::check::check;
use crate::cx::Cx;
use crate::io::Io;
use crate::sched::Sched;

pub use baton::Baton;
pub use latch::Latch;
pub use mutex::{Mutex, MutexGuard};
pub use scope::{Scope, scope};
pub use semaphore::Semaphore;

mod sealed {
    use std::rc::Rc;

    use crate::sched::Sched;

    pub trait Sealed {
        fn sched(&self) -> &Rc<Sched>;
    }
}

/// Something a primitive can be bound to: an [`Io`] (so shared state can be
/// built before any fiber runs) or a [`Cx`]. Sealed.
pub trait Attach: sealed::Sealed {}

impl sealed::Sealed for Io {
    fn sched(&self) -> &Rc<Sched> {
        Io::sched(self)
    }
}
impl sealed::Sealed for Cx {
    fn sched(&self) -> &Rc<Sched> {
        Cx::sched(self)
    }
}
impl Attach for Io {}
impl Attach for Cx {}

#[inline]
pub(super) fn bind(at: &impl Attach) -> Rc<Sched> {
    Rc::clone(sealed::Sealed::sched(at))
}

/// Rule 13: a primitive built on one `Io` must not be used from another.
#[inline]
pub(super) fn check_same(bound: &Rc<Sched>, cx: &Cx, what: &str) {
    check!(
        Rc::ptr_eq(bound, cx.sched()),
        "{what} used from a different Io than the one it was created on"
    );
}
