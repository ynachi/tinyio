#![forbid(unsafe_code)]
//! `Io`: owns the thread's scheduler (and, from stage 2, the ring). `!Send`:
//! create it on the thread that runs it. Holds owning handles only (Rule 12):
//! nothing the kernel or a fiber points at lives inline, so `let io2 = io;`
//! is harmless.

use std::io;
use std::marker::PhantomData;
use std::rc::Rc;

use crate::cx::{Cx, NotSend};
use crate::detail::fiber::FiberId;
use crate::options::Options;
use crate::sched::Sched;

pub struct Io {
    sched: Rc<Sched>,
    _p: NotSend,
}

impl Io {
    pub fn new(opts: Options) -> io::Result<Io> {
        let opts = opts.validate()?;
        Ok(Io {
            sched: Sched::new(opts),
            _p: PhantomData,
        })
    }

    #[cfg(test)]
    pub(crate) fn sched(&self) -> &Rc<Sched> {
        &self.sched
    }

    /// Queue a fiber. It starts when the loop next runs.
    pub fn spawn<F>(&self, f: F) -> io::Result<FiberId>
    where
        F: FnOnce(&Cx) + 'static,
    {
        self.sched.spawn(f)
    }

    /// Run fibers until none is ready (stage 1; from stage 2 on: until
    /// nothing is ready, in flight or posted, invariant 9). A fiber still
    /// live then can never be woken, which is a bug: it aborts, naming the
    /// primitive (invariant 10).
    pub fn run_until_done(&mut self) {
        let batch = self.sched.opts().batch_max_size;
        while !self.sched.quiesced() {
            self.sched.resume_ready(batch);
        }
        self.sched.abort_if_started_live("quiesced");
    }
}

impl Drop for Io {
    /// Never runs fibers (section 6, "Shutdown"). Aborts if a started fiber
    /// is unfinished; drops never-started closures without running them.
    fn drop(&mut self) {
        self.sched.teardown();
    }
}
