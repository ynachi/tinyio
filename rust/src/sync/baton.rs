use std::cell::Cell;
use std::io;
use std::rc::Rc;

use super::waiters::WaitList;
use super::{Attach, bind, check_same};
use crate::cx::Cx;
use crate::detail::fiber::WaitKind;
use crate::sched::Sched;

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Empty,
    Waiting,
    Posted,
}

/// A one-shot signal for one waiter: `post` is sticky and idempotent, `wait`
/// after it returns at once. There is no reset; a new wait is a new `Baton`.
pub struct Baton {
    sched: Rc<Sched>,
    state: Cell<State>,
    waiters: WaitList,
}

impl Baton {
    #[inline]
    pub fn new(at: &impl Attach) -> Baton {
        Baton {
            sched: bind(at),
            state: Cell::new(State::Empty),
            waiters: WaitList::new(WaitKind::Baton),
        }
    }

    /// Wait for `post`; returns at once if it has happened. `ECANCELED` if the
    /// runtime is stopping (a producer that was cancelled will never post).
    /// Aborts on a second concurrent waiter: one-shot means one.
    #[inline]
    pub fn wait(&self, cx: &Cx) -> io::Result<()> {
        check_same(&self.sched, cx, "Baton");
        match self.state.get() {
            State::Posted => Ok(()),
            State::Waiting => crate::check::fatal(format_args!(
                "Baton: second concurrent wait (one-shot means one waiter)"
            )),
            State::Empty => {
                self.state.set(State::Waiting);
                let woke = self.waiters.wait(&self.sched);
                if woke.is_err() {
                    self.state.set(State::Empty); // nobody waits any more
                }
                woke
            }
        }
    }

    #[inline]
    pub fn is_posted(&self) -> bool {
        self.state.get() == State::Posted
    }

    #[inline]
    pub fn post(&self) {
        if self.is_posted() {
            return;
        }
        self.state.set(State::Posted);
        // Last use of `self`: the waiter may free the baton once it runs.
        self.waiters.wake_one(&self.sched);
    }
}
