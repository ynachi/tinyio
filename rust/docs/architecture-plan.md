# tinyio-rs: Invariant-First Architecture Plan

Oct 31, 2026 · @Yao

## Purpose and what changed

This plan rebuilds tinyio as idiomatic Rust around the invariants you earned in C++, not around its code, and adds one hard rule: a user of the crate never writes `unsafe`.

**How to use it with a coding model.** Paste sections 2 (the rules) and 4 (the API) into every task, add the stage from section 9, and attach the rows of section 3, the matching subsection of section 6 and the rows of section 7 that the stage touches. Section 7 is also the review checklist for anything unsafe.

**What changed from the first plan:**

- Invariants lead and code is a sketch. The C++ `docs/core-reading-guide.md` invariants 1–11 are the spec; each now says how Rust enforces it.
- Parked fibers are never woken early. I/O waits end on the CQE and sync waits end on their primitive, so the cancel hooks, early-wake path and Rule 6 are gone. Per-fiber cancellation is not in v1.
- `Io` (owner thread, `!Send`), `Cx` (handed to each fiber) and `Remote` (the only `Send` handle) replace thread-local scheduler globals and the `RefCell` hazard.
- `crossbeam_queue::ArrayQueue` replaces the hand-rolled Vyukov queue, so the `Inconsistent` lost-wakeup case cannot occur.
- `std::io::Error` replaces the custom error enum.
- Sync primitives own their data (`Mutex<T>`), and task groups are scoped like `std::thread::scope`.
- New: the safe-API rule, a soundness checklist, and a staged build with death tests.

**Ported, not rewritten:** the context-switch assembly, ported from C++ detail/context.cc (not from the first plan). I ran the x86\_64 version: RSP % 16 is 8 at the first instruction of the entry function, aligned-SSE code runs on the fiber stack, and `catch_unwind` works there. The aarch64 version was only checked by hand against its frame layout. Saving FP control state (MXCSR, x87 control word, FPCR) is part of the stage-1 port, as is the unwind CFI marker. Both are in C++ `context.cc`.

**Revised after an independent review (rev 20):**

- Rules 12 and 13 added: stable addresses (a moved `Io` must not corrupt memory) and fiber identity (a scoped child holding its parent's `Cx` must not wake the wrong fiber).
- Every stage prompt now carries sections 4, 6 and 7, and the assembly is ported from C++ `context.cc`, with FP state and CFI in stage 1.
- `sleep` is a Timeout SQE and moves to stage 2. `Drop for Io` never runs fibers. The timer wheel uses a slab with O(1) unlink.
- `Scope` markers are specified, `GETEVENTS` and `AsyncCancel2` are explained for `io-uring` 0.7, and the thread-local limit is documented.

## Rules for the coding agent

Every file you write obeys these rules. If a task seems to need breaking one, stop and report it instead of working around it.

**Rule 0: the public API is safe.** Every `pub` item reachable from the crate root is safe to call, and no sequence of calls to tinyio's safe API can cause undefined behavior. The one stated boundary is in section 7: third-party code that keeps a scoped thread-local across a suspend.

- No `pub unsafe fn`, no `pub unsafe trait`, and no public type whose docs carry a safety contract.
- All `unsafe` lives in the modules listed in section 5. Every `unsafe` block has a `// SAFETY:` comment naming the invariant it relies on.
- Crate lints: `#![deny(unsafe_op_in_unsafe_fn)]`, clippy `undocumented_unsafe_blocks` and `missing_safety_doc` as errors.
- Integration tests, examples and benches start with `#![forbid(unsafe_code)]`. They are the proof that the API is usable without `unsafe`.
- When safety depends on something the types cannot see, check it at runtime and abort with a message. Never leave it as a documented precondition.
- If an operation cannot be made safe, it is not public. Keep it `pub(crate)` or change the design.

**Rule 1: thread confinement is a type.** `Io`, `Cx` and every sync primitive are `!Send + !Sync` (a `PhantomData<*const ()>` field). The only `Send + Sync` handle is `Remote`; closures posted through it must be `Send + 'static`.

**Rule 2: fibers never migrate.** A fiber runs on the thread that spawned it, for life. Code may cache thread-local addresses across a context switch only because of this.

**Rule 3: wake a parked fiber only through what it parked on.** An I/O wait ends on its CQE and nothing else. A sync wait ends when its primitive unparks it. The runtime never force-wakes either, at shutdown or otherwise.

**Rule 4: one way into the ready queue.** Use `push_ready`. It does nothing if the fiber is already `Ready` and asserts it is not `Done`.

**Rule 5: no `RefCell` or `Mutex` in scheduler or ring state.** Use `Cell` and `UnsafeCell` inside small wrapper types whose methods never return a reference that lives across a context switch. Intrusive links are `Cell<Option<NonNull<T>>>`. Never create `&mut` to a node once it is linked.

**Rule 6: decode `user_data` in one place.** One function turns the `u64` into `enum UserData { Fiber(NonNull<Completion>), Wake, Cancel }`. Pack the tag in the low two bits of an 8-byte-aligned pointer, or use sentinel values; either is fine, but only that function knows.

**Rule 7: the loop order is fixed.** Each tick is resume, then submit-or-wait, then reap, with the quiesce check before any blocking wait (section 6). Block in the kernel only when a CQE, an eventfd write or the nearest timer deadline can end the wait.

**Rule 8: panics abort.** A panic that escapes a fiber aborts the process, which matches the C++ `noexcept` task. Internal invariant failures log and call `std::process::abort()`; they never use `panic!`. Do not add `catch_unwind` unless a task asks for join semantics.

**Rule 9: no tracing spans inside fibers.** An entered span lives on the thread's span stack and leaks into whichever fiber runs next. Log events with a `fiber = id` field instead. `tracing` stays an optional feature and the crate never depends on `tracing-subscriber`.

**Rule 10: do not edit the assembly** in `detail/context.rs` after stage 1. FP-state saving and the unwind CFI marker are part of that port from the start.

**Rule 11: one stage at a time.** Finish a stage, run its verification command, and stop. Do not start the next stage's files.

**Rule 12: nothing the kernel or a fiber points at lives inline in `Io`.** Safe code can move `Io` (`let io2 = io;`), so every address handed out must be stable. The eventfd read target lives in `Arc<Shared>`, the scheduler behind an `Rc<Sched>`, and `Completion` and fiber headers on fiber stacks. `Io` itself holds only owning handles. Test: run, move the `Io`, post from another thread, run again.

**Rule 13: a fiber's identity is `sched.current`, never the `Cx` it was given.** A scoped child can legally hold its parent's `&Cx`, so `Cx` carries only the scheduler handle. Every park and unpark checks the fiber's state: unpark requires `Waiting(kind)` with the caller's kind and never `Io`, and `push_ready` rejects `Running`. A mismatch aborts with both kinds named. A mutex is owned by its guard, not by a fiber, so a guard may be dropped by another fiber on the same thread.

## Invariants and how Rust enforces them

Each invariant has one enforcement point. The type system carries 1, 2 and 6; the rest are runtime checks or aborts at one named place. Numbers 1–11 are the C++ guide's; 12 and 13 are new.

| # | Invariant | Rust enforcement | Kind |
| --- | --- | --- | --- |
| 1 | A fiber has at most one op in flight | `Completion` is a local in the `syscall` frame. The call blocks the fiber until the CQE, so a second op cannot start. | structure |
| 2 | An op and its buffers outlive every kernel reference | Buffers are `&[u8]` / `&mut [u8]` borrowed for the blocking call, in the same frame as `Completion`. `Io::drop` runs cancel-and-drain and aborts if it cannot reach quiescence before freeing the ring and stacks. Nothing the kernel can write to lives inline in Io (Rule 12). | borrow checker + abort |
| 3 | `inflight_ops` counts submissions that still owe a terminal CQE | `Cell<usize>` in `Ring`. +1 per submit (op, timeout cancel, drain cancel), −1 per CQE, never for the wake read. | runtime assert |
| 4 | `user_data` has three meanings | `UserData::decode`, the only code that reads the raw `u64` | one function |
| 5 | A fiber is in the ready queue at most once | `push_ready` guard on `FiberState` | runtime |
| 6 | Only the owner thread touches ring, fibers, queues (except `Post`/`Wake`) | `Io` and `Cx` are `!Send`. `Remote` is the only cross-thread handle. | compile time |
| 7 | A cancel is submitted before its target op can be reused | Timeout and drain cancels are submitted right after they are prepared, before any fiber resumes. | ordering + test |
| 8 | The loop blocks only when something can wake it | `submit_or_wait` blocks only with empty ready queue, empty CQ and no pending posts. The wait is bounded by `timers.ns_until_next()`. | runtime |
| 9 | Quiesced = nothing ready, nothing in flight, nothing posted; live fibers do not count | One `quiesced()` function used by the tick and by drain | one function |
| 10 | The runtime never wakes a sync-parked fiber | The only unpark path is `pub(crate)` inside `sync`. Unpark requires the fiber to be Waiting on the caller's kind, never Io (Rule 13); a mismatch aborts. A fiber still live at quiesce aborts with its `WaitKind` named. | abort |
| 11 | During a drain, `inflight_ops` only goes down | `stopping` flag. `syscall` and `yield_now` return `ECANCELED` before submitting. | runtime |
| 12 | Safe code cannot cause UB (new) | Rule 0, section 7 checklist, `compile_fail` tests | lints + tests |
| 13 | No post succeeds after the loop closed its queue (new) | `post_state` CAS with a closed bit, as in C++ | runtime |

Two consequences worth knowing before coding:

- Because of 2 and 10, a fiber that is parked forever at shutdown is a bug and aborts. It is never unwound, which would run destructors under a live kernel reference.
- Because of 3, every place that submits an SQE must increment `inflight_ops` through the one `Ring::submit` helper, not by hand.

## Public API sketch

The API has three handles: `Io` owns the thread, `Cx` is what a fiber holds, and `Remote` is what other threads hold. I type-checked this sketch with `todo!()` bodies, and the six claims in the list under it hold as compile errors, each failing for the reason named.

&#91;embedded content: threads and handles · 3 handles, 1 cross-thread path\]

Other threads reach the runtime only through `Remote` and the shared queue; the owner thread resumes fibers and fibers call back into it through `Cx`.

```
type NotSend = PhantomData<*const ()>;           // makes a type !Send + !Sync

pub struct Io { _p: NotSend }                    // owner thread; !Send; owning handles only (Rule 12)
pub struct Cx { _p: NotSend }                    // handed to each fiber; !Send; scheduler handle only (Rule 13)
#[derive(Clone)] pub struct Remote { /* Arc<Shared> */ }   // Send + Sync

impl Io {
    pub fn new(opts: Options) -> io::Result<Io>;
    pub fn spawn<F>(&self, f: F) -> io::Result<FiberId> where F: FnOnce(&Cx) + 'static;
    pub fn remote(&self) -> Remote;
    pub fn run_until_done(&mut self);            // until quiesced
    pub fn run(&mut self);                       // until Remote::stop, then cancel + drain
}
impl Remote {
    pub fn post<F>(&self, f: F) -> Result<(), PostError>
        where F: FnOnce(&Cx) + Send + 'static;   // Err is Full or Closed; the closure is dropped
    pub fn stop(&self);
}
impl Cx {
    pub fn spawn<F>(&self, f: F) -> io::Result<FiberId> where F: FnOnce(&Cx) + 'static;
    pub fn yield_now(&self) -> io::Result<()>;
    pub fn sleep(&self, d: Duration) -> io::Result<()>;   // a Timeout SQE, counted in flight; -ETIME is Ok
    pub fn read(&self, fd: &impl AsFd, buf: &mut [u8], offset: Option<u64>) -> io::Result<usize>;
    pub fn write(&self, fd: &impl AsFd, buf: &[u8], offset: Option<u64>) -> io::Result<usize>;
    pub fn accept(&self, l: &impl AsFd) -> io::Result<OwnedFd>;
    pub fn timeout(&self, d: Duration) -> Timed<'_>;   // same I/O methods, ETIMEDOUT after d
}
pub fn is_cancelled(e: &io::Error) -> bool;      // raw_os_error() == ECANCELED

pub struct Mutex<T>;                             // owns T; !Send
impl<T> Mutex<T> {
    pub fn new(at: &impl Attach, v: T) -> Self;  // Attach is sealed; Io and Cx implement it, so a Mutex can be built before any fiber runs
    pub fn lock<'a>(&'a self, cx: &Cx) -> MutexGuard<'a, T>;   // Deref/DerefMut
    pub fn try_lock(&self) -> Option<MutexGuard<'_, T>>;
}

pub fn scope<'env, F, R>(cx: &Cx, f: F) -> R     // like std::thread::scope
    where F: for<'scope> FnOnce(&'scope Scope<'scope, 'env>) -> R;
pub struct Scope<'scope, 'env: 'scope> {
    _scope: PhantomData<&'scope mut &'scope ()>, // invariant, exactly as in std
    _env: PhantomData<&'env mut &'env ()>,       // invariant, exactly as in std
    _p: NotSend,
}
impl<'scope, 'env> Scope<'scope, 'env> {
    pub fn spawn<F>(&'scope self, f: F) -> io::Result<FiberId> where F: FnOnce(&Cx) + 'scope;
}
```

**What the compiler now rejects** (all checked):

- Moving `Io` or a `Mutex` to another thread.
- Capturing a `Cx` inside a spawned fiber, which would let it escape its owner.
- Posting a closure that captures an `Rc` or any other non-`Send` value.
- Spawning a plain fiber that borrows the parent's locals. Only `scope` allows that, and it cannot leak a borrow past its end.
- Holding a `MutexGuard` after its mutex is gone.
- A scoped child that borrows a local declared inside the scope closure. This is why `Scope` carries invariant markers; with covariant ones it compiles and reads freed memory.

**Design choices to keep:**

- There is no global "current runtime". I/O methods hang off `Cx`, so "inside a fiber" is a type, not a `CHECK`.
- Errors are `std::io::Error`. It is 8 bytes and allocation-free for OS errors, and `Result<usize, io::Error>` is 16 bytes. Map the C++ rule `-ECANCELED` with `timed_out` to `ETIMEDOUT` in `syscall`; `ECANCELED` has no `ErrorKind`, so callers match `raw_os_error()`.
- Timeouts are a view (`cx.timeout(d)`) over the same methods, not a parameter on each one.
- Sync primitives and `scope` are `!Send`. They hold a handle to the scheduler so that dropping a guard can unpark the next waiter without a `Cx`.
- `net` uses `std::net::SocketAddr` and `OwnedFd`. For `std::io::Read`/`Write` interop, offer an adapter that binds a stream and a `Cx` (`stream.with(cx)`), because the traits have nowhere to take a `Cx`.

## Module layout and unsafe budget

`unsafe` is allowed in eight places only. Everything else carries `#![forbid(unsafe_code)]` in its module, so a reviewer audits the table below and nothing more.

```text
src/
  lib.rs          re-exports only; #![deny(unsafe_op_in_unsafe_fn)]
  io.rs           Io: owns Sched + Ring + Timers; run loop; Drop = cancel + drain
  cx.rs           Cx, Timed, and syscall(), the one path every I/O op takes
  remote.rs       Remote, Shared { ArrayQueue, post_state, eventfd: OwnedFd }
  sched.rs        ready queue, fiber registry, resume / suspend / finish, spawn
  ring.rs         io_uring wrapper: submit with retry, drain CQEs, UserData, inflight
  timer.rs        timer wheel over opaque keys; never dereferences a fiber
  sync/           waiters.rs  mutex.rs  semaphore.rs  latch.rs  baton.rs  scope.rs
  net/            listener.rs  stream.rs  sockopt.rs
  buffers.rs      fixed buffer pools (stage 6)
  detail/         context.rs  stack.rs  fiber.rs
```

| Module | Why it needs `unsafe` | What makes it sound | Public? |
| --- | --- | --- | --- |
| `detail/context.rs` | naked asm, raw stack pointers | Frame layout fixed. The entry fn never returns. Verified on x86\_64. | no |
| `detail/stack.rs` | `mmap`, `mprotect`, `munmap` | Guard page below. A stack is freed only when its fiber is `Done` and switched out. | no |
| `detail/fiber.rs` | `NonNull<FiberHeader>`, closure stored in the stack, type-erased runner | Header sits at the stack top until the stack is released. The closure is written once, run once, dropped once. | no |
| `sched.rs` | calls `swap_context`; ready queue of `NonNull` | Single thread (rules 1, 2). `push_ready` guard. No reference is held across a switch. | no |
| `ring.rs` | SQEs carry raw pointers; a CQE becomes `NonNull<Completion>` | The `Completion` is on a fiber stack that stays parked until its CQE. Drain before free. | no |
| `cx.rs` (`syscall`) | builds an SQE from the caller's buffer pointer | The buffer is borrowed for the whole call and the fiber is blocked until the CQE. | `Cx::read` etc. are safe |
| `sync/waiters.rs` | intrusive waiter nodes on parked stacks | A node is unlinked before its fiber resumes. A primitive dropped with waiters aborts. | primitives are safe |
| `sync/scope.rs` | erases `'scope` to `'static` to store the child closure | `scope` does not return until every child is `Done`. Unwinding through it aborts. | `scope` is safe |

These modules are forbidden from `unsafe` on purpose:

- `remote.rs` leans on `ArrayQueue`, `Arc` and `OwnedFd`. Because the `Arc<Shared>` owns the eventfd, a late `Remote::stop` can never write to a closed or recycled fd, so the C++ `wake_mutex_` is not needed.
- `timer.rs` stores opaque keys and returns a `TimerId`. A fiber keeps its `TimerId` in a `Cell`, and entries live in a slab and each wheel slot is a doubly linked list of slab indices, so disarm unlinks in O(1) and the wheel never touches a fiber. A TimerId carries a generation, and an expired entry with a stale generation aborts (always on).
- `net/` and `io.rs` only call safe functions.

CI check: `grep -R "pub unsafe" src` must be empty, and `cargo public-api` output must contain no `unsafe`.

## Core mechanics

This section specifies behavior, not code. Where a line of Rust is shown it is a sketch of the shape.

### Fibers

A fiber is one `mmap` region: a `PROT_NONE` guard region (guard\_size, default 64 KiB, at least one page), the usable stack growing down, the initial frame, the task closure, and the `FiberHeader` (64-byte aligned) at the very top. Spawning allocates nothing beyond the mapping, which is recycled through an idle list (LIFO, capped by `max_idle_fibers`).

- **States:** `Ready` (exactly "in the ready queue"), `Running`, `Waiting`, `Done`. A `Cell<WaitKind>` records why a fiber waits: `Io`, `Mutex`, `Semaphore`, `Latch`, `Baton`, `Scope`, `BufferPool`. It costs nothing and names the culprit when shutdown aborts.
- **Registry:** `Sched` keeps a `Vec<NonNull<FiberHeader>>` of every fiber, idle ones included. Each header stores its index for swap-and-pop removal. The abort report and `destroy_all` need it.
- **Spawn:** take an idle fiber or map a new one. Write the closure at the stack top (an error if it is larger than half the stack or needs alignment above 64). Build the initial frame with `Context::init_stack`. Give it a fresh id, set `Ready`, push, and increment `live`.
- **Resume:** set `current`, state `Running`, swap from the scheduler context into the fiber, clear `current` on return. If the state is now `Done`, recycle the fiber. The scheduler context is an `UnsafeCell<Context>`.
- **Entry:** `extern "C" fn fiber_entry(arg) -> !` runs the closure by value, marks `Done`, and does a last switch out. It never returns; if it does, abort. A panic escaping it aborts (Rule 8).
- **Suspend:** switch to the scheduler context. No `Ref` or `&mut` of scheduler state is alive across this call.
- **Never-started fibers** are dropped through a type-erased dropper at teardown, so their closures' destructors still run.

### One I/O call

Every operation goes through one function, `Cx::syscall`. Its steps:

1. If `stopping`, return `ECANCELED` without touching the ring (invariant 11).
2. Get an SQE. If the SQ is full, submit what is queued and try once more; still none means `ENOSPC`.
3. Create the `Completion { fiber, result: Cell<i32> }` as a local, 8-byte aligned. Its address is the `user_data`. Reset the header's `timed_out` flag.
4. Prepare the SQE from the caller's buffer, then set `user_data`.
5. With a timeout, arm the timer wheel for `now + d`. No SQE is used.
6. Set the state to `Waiting(Io)`, increment `inflight`, and suspend.
7. The fiber resumes only from `drain_completions`, which writes `result`, disarms the timer and calls `push_ready`.
8. A negative result maps to `io::Error::from_raw_os_error(-res)`, except `-ECANCELED` with `timed_out` set, which becomes `ETIMEDOUT`. Otherwise return `res as usize`.

Because nothing else can resume a fiber parked here, there is no "woken early while the kernel still holds my stack" path and no uninterruptible re-park loop. If per-fiber cancellation is added later, it must keep this property: the cancel is an `ASYNC_CANCEL` and the fiber still resumes on its CQE.

### The event loop

The loop is the C++ pipeline: resume, then submit-or-wait, then reap. The quiesce check sits before the blocking wait; without it, the last fiber finishing leaves the loop waiting for a CQE that never comes.

```rust
fn tick(&mut self) -> io::Result<()> {
    self.clock.invalidate();
    self.drain_remote();                  // posted closures become fibers, bounded per tick
    self.resume_ready();                  // up to batch_max_size fibers
    if self.draining && self.quiesced() { // BEFORE any blocking wait
        return Ok(());
    }
    let submitted = self.submit_or_wait();
    self.drain_completions();             // even if submit failed: CQEs already posted must wake fibers
    self.process_expired_timers();        // after the drain, so finished ops are already disarmed
    submitted
}
```

- **`submit_or_wait`:** the loop is runnable if the CQ has entries, the ready queue is non-empty, or posts are pending. A runnable tick submits with `GETEVENTS` set, which `DEFER_TASKRUN` requires. Otherwise it submits and waits for one CQE, bounded by `timers.ns_until_next()`. `EINTR` and `ETIME` are success. `EBUSY` and `EAGAIN` mean the CQ overflowed: reap, then continue.
- **Submit retry:** up to four attempts. Retry `EINTR`; on `EBUSY`/`EAGAIN` reap once and retry.
- **`drain_completions`:** decode each CQE with `UserData::decode`. `Wake` is never counted and sets "re-arm the eventfd read" (skipped when stopping). `Cancel` decrements `inflight`. `Fiber` decrements `inflight`, stores the result, disarms the timer and calls `push_ready`. Re-arm only after `cq_advance`, never while iterating.
- **Ring setup:** `SINGLE_ISSUER | DEFER_TASKRUN | TASKRUN_FLAG`, plus `R_DISABLED`, `CQSIZE` (CQ = 2 × SQ) and `CLAMP`. Enable the ring on the thread that calls `run`. Kernel floor is 6.1, with no fallback.
- **Tick errors:** `run` stops after three consecutive failed ticks and drains. A drain that keeps failing aborts, because tearing the ring down under in-flight ops is a kernel use-after-free.

**`GETEVENTS` and cancels in safe code (`io-uring` 0.7).** `submit()` and `submit_and_wait(0)` set `GETEVENTS` only when waiting for at least one CQE, or when the CQ overflowed, and `Submitter::enter` is `unsafe`. A runnable tick therefore calls `submit_with_args(1, &SubmitArgs::new().timespec(&ZERO))` and treats `ETIME` as success; a blocking tick passes the timer bound the same way. `AsyncCancel` has no flags field: the drain cancel uses `AsyncCancel2` built with `CancelBuilder::any()` (ANY already matches every request), and a single cancel uses `AsyncCancel` on one `user_data`. Stage 2 has a test that a pipe write made while a fiber is ready lets the reader run on the next tick without blocking.

### Timeouts

Arming costs no SQE and no kernel timer. The kernel gets involved only when a timeout fires.

- A fiber with a timeout holds a `TimerId` in a `Cell`. The wheel stores an opaque fiber key and a deadline in a slab. Disarm unlinks the entry and frees its slab index with the generation bumped, so finished ops leave nothing behind.
- When `process_expired_timers` finds an expired entry it gets an SQE, sets the header's `timed_out`, prepares `AsyncCancel` on the op's `user_data` (kept in the header as `op_user_data`), tags it `Cancel`, and increments `inflight`. If no SQE is available it re-arms one tick later.
- It then submits immediately. This is invariant 7: the cancel must reach the kernel before the fiber's next op can reuse the same `user_data`.
- Any wheel that arms and disarms in O(1) and reports `ns_until_next` is acceptable. Slot width may be coarse (the C++ uses 64 ms); say so in the docs, because a timeout can fire up to one slot late.

**`sleep` is not a wheel timer.** As in C++, `sleep` goes through `syscall` as a Timeout SQE. It counts in `inflight`, so the quiesce check sees a sleeping fiber and does not mistake it for a parked one. `-ETIME` is success, a stop returns `ECANCELED`, and a zero or negative duration returns `Ok` without touching the ring. The wheel serves only the per-op timeout of `cx.timeout(d)`.

### Posting from other threads

`Shared` holds an `ArrayQueue<Box<dyn FnOnce(&Cx) + Send>>`, an `AtomicU64` `post_state` (low 63 bits = pending count, top bit = closed), the eventfd as an `OwnedFd`, and the stop flag.

1. `post` CASes `post_state` to add one unless the closed bit is set (invariant 13).
2. It pushes the box. If the queue is full it subtracts one and returns `PostError::Full`; the closure is dropped.
3. It writes the eventfd after the push. `EAGAIN` is fine, since a saturated counter still wakes the reader.
4. The owner treats `pending > 0` as runnable even if the queue looks empty: a producer between steps 1 and 2 means "keep ticking, do not sleep".
5. `drain_remote` pops at most `batch_max_size` per tick, subtracts the number it popped from the pending count, and spawns each closure as a fiber.

`ArrayQueue::pop` spins briefly when a producer has claimed a slot but not finished writing it. A producer preempted at that moment stalls the owner thread for up to a scheduling quantum. Accept it and document it; the pending count still keeps the loop from sleeping.

### Shutdown

Shutdown is where every invariant is tested at once, so it follows one fixed sequence.

1. `run` ticks until `Remote::stop` sets the stop flag and writes the eventfd, or until three consecutive ticks fail.
2. Set the closed bit on `post_state`, then set `stopping`.
3. **Cancel and drain.** If `inflight > 0`, submit one `AsyncCancel` with `CancelBuilder::any()` (tagged `Cancel`, counted). Then tick in drain mode until nothing is in flight, ready or posted. The eventfd read is cancelled too; `stopping` suppresses its re-arm.
4. **Abort if parked.** If `live > 0` after quiesce, log each parked fiber's id and `WaitKind`, then abort. Nothing can wake those fibers, so unwinding them would run destructors under live borrows.
5. Discard closures still in the post queue.

`Drop for Io` never runs user fibers. It aborts, naming the parked fibers, if `inflight` is not zero or a started fiber is still live. After `run` or `run_until_done` has returned that cannot happen, so the abort is a seatbelt. Closures of never-started fibers are dropped without running. Teardown order: ring, those closures, stacks. `Io` is `!Send`, so it must be created on the thread that runs it. That removes the C++ "destroyed from the wrong thread" abort, and it matches `SINGLE_ISSUER`.

**Edge cases (recommended behavior; settle them in stage 4):**

- After `run` has drained, the `Io` is stopped: `run` and `run_until_done` return at once and `spawn` returns `ECANCELED`. Running, spawning and running again before any stop is supported (the C++ `DoubleRun` test).
- `Cx::spawn` while stopping returns `ECANCELED`, like `syscall` and `yield_now`, so a spawning loop cannot keep the drain alive.
- `Remote::stop` during `run_until_done` sets the same sticky flag. `run_until_done` then performs the cancel-and-drain and returns.
- If re-arming the eventfd read fails (SQ full), keep `rearm_pending` set and treat the loop as runnable until it succeeds. Never block with the read unarmed.

## Soundness checklist for the safe API

Rule 0 holds only if each way safe code could cause UB is closed by a type, a runtime abort, or by being impossible. Every row needs a test (section 8) before its stage is done.

| Hazard | How it is closed | Checked by |
| --- | --- | --- |
| Buffer freed or moved while the kernel holds its pointer | The buffer is borrowed for a call that blocks until the CQE. `Io::drop` drains before freeing stacks. `mem::forget(Io)` leaks the ring and stacks, which keeps them mapped. | borrow checker; drop test |
| `Cx` outlives its fiber or crosses threads | `Cx: !Send`, and a `&Cx` cannot be captured by a `'static` closure | `compile_fail` |
| Primitive or `Cx` from one `Io` used in another `Io` on the same thread | Primitives store the scheduler handle. `lock`/`wait` compare it with `cx`'s and abort on mismatch. | unit test (abort) |
| Primitive dropped while a fiber is parked on it | The parked fiber holds a borrow. `Drop` with waiters aborts as a second line. | ownership + abort test |
| Guard outlives its mutex | `MutexGuard<'a, T>` borrows the mutex | `compile_fail` |
| A scoped child outlives the data it borrows | `scope` waits for every child. A panic or shutdown inside it aborts, so frames never die under a live child. | `compile_fail` + abort test |
| Closure posted from another thread holds a non-`Send` value | `F: Send + 'static` on `Remote::post` | `compile_fail` |
| Runtime driven from inside a fiber | `Cx` has no access to `Io`, and `run` takes `&mut Io` | by construction |
| Parked fiber unwound at shutdown, running destructors under live borrows | Never woken; quiesce with live fibers aborts | death test |
| Fiber stack overflow | A guard region (default 64 KiB) plus Rust's stack probes make it a SIGSEGV for any frame the probes cover. C or asm frames without stack-clash protection can skip a small guard, so keep it large. A `sigaltstack` handler that prints "fiber stack overflow" and aborts is optional. | doc + test |
| Pointer ↔ `user_data` provenance | Convert with `expose_provenance` and `with_exposed_provenance`, only inside `UserData` | code review |
| Read or write longer than the kernel allows | Clamp to `0x7fff_f000`; the call returns a short count | unit test |
| A fiber ends with its timer still armed | `destroy_fiber` aborts if the fiber still holds a TimerId (always on) | abort test |
| `Remote` used after `Io` is gone | `post` returns `Closed`. The `Arc<Shared>` keeps the eventfd alive. | unit test |
| Io is moved after the kernel holds a pointer into it | Rule 12: the eventfd read target lives in Arc\<Shared>, the scheduler behind an Rc. Io holds owning handles only. | test: run, move, post, run |
| A scoped child uses its parent's &Cx, or a guard is dropped by another fiber | Rule 13: the acting fiber is always sched.current. Unpark checks the wait kind and aborts on a mismatch. | positive test + abort test |
| A scoped child borrows a local that dies before the join | Scope carries invariant markers for 'scope and 'env, exactly as std::thread::Scope does. | compile\_fail |

**Documented limits:**

- A blocking syscall inside a fiber stalls the whole thread.
- Holding the standard library's `std::sync::Mutex` (not tinyio's `Mutex`) across a suspend can deadlock: a second fiber on the same thread blocks the whole OS thread in `lock()`. Use tinyio's `Mutex` inside fibers.
- A `thread_local!` is shared by every fiber on the thread. Scoped thread-local APIs (set, run, restore) assume LIFO nesting, so two fibers interleaving them can read a dead fiber's stack. That is UB reachable from third-party safe code and tinyio cannot prevent it, as with any stackful fiber library. The docs say: do not hold a scoped thread-local across a suspend.
- Dropping an `OwnedFd` closes it synchronously; prefer the async close once it exists.

**Review questions for any `unsafe` block.** Each answer goes in the `// SAFETY:` comment.

1. Which invariant from section 3 makes this sound?
2. Which type or check would stop safe code from breaking that invariant?
3. Does any reference created here live across a context switch? It must not.

## Test strategy

Tests are the specification of the invariants, grouped by invariant rather than ported file by file. Write each test's expected outcome in a comment before running it; a wrong prediction marks a part you do not own yet.

**Layers:**

1. **Unit tests, no ring.** Intrusive wait list (FIFO), ready queue (the double-push guard from invariant 5), timer wheel (arm, disarm, `ns_until_next`, deadline aliasing), `UserData` round trip, `post_state` CAS protocol.
2. **Fiber tests, no ring.** Switch in and out, spawn and recycle with a fresh id each time, a never-started fiber's closure is dropped, a large capture lands on the fiber stack, spawn fails cleanly when the closure is over half the stack.
3. **I/O tests, Linux 6.1+.** Files in a temp dir, pipes and socketpairs: short reads, offsets, timeouts, accept and connect, and a fiber that finishes with its timer still armed (must not happen).
4. **Death tests.** A subprocess harness: the test re-executes its own binary with `TINYIO_CASE=<name>` and asserts an abort signal and expected stderr text. Cases: quiesce with a fiber parked on a `Mutex` names `Mutex` and the fiber id; two stuck primitives are both named; a primitive dropped with waiters aborts; `Io` dropped with a parked fiber (through the test hook) aborts and names it; Io dropped after run\_until\_done is clean.
5. **Shutdown tests that must pass.** Stop token cancels fibers waiting in I/O (the quiesce check); stop cancels ready fibers; mutex handoff during shutdown keeps ownership; a scope's parent is woken only after its cancelled children finish.
6. **`compile_fail` doctests.** The six cases in section 4 plus the scope and guard cases in section 7. Each ships with a passing twin so a typo cannot make it pass for the wrong reason.

**Predict, then run.** Predictions follow from the invariants:

| Scenario | Predicted outcome |
| --- | --- |
| A fiber loops on `yield_now()` and ignores the error, then stop is requested | `yield_now` fails with `ECANCELED`; the loop never leaves phase 1, so the drain spins. Document it; test only that fibers which check the error exit. |
| Two fibers read one socket with 1 s timeouts and data arrives at 0.99 s | One fiber gets the data. The other gets `ETIMEDOUT`, up to one wheel slot late. |
| The listening fd is closed while a fiber is parked in `accept` | Not writable in safe code: the parked call holds a borrow of the fd. Add a `compile_fail` for it. |
| 8 threads call `Remote::post` while the owner is blocked in `submit_and_wait` | All posts run. The eventfd wakes the owner. |
| Remove the immediate submit in `process_expired_timers` | A fiber that times out and immediately starts a second op on another pipe has that second op cancelled by the late cancel. The test asserts it completes with data. |
| `guard_size = 0` and deep recursion | Rejected by `Options` validation: the guard is at least one page. |
| let io2 = io; after run\_until\_done, then post from another thread and run again | The post runs. Nothing the kernel writes to lives inline in Io (Rule 12). |
| A scoped child calls m.lock(parent\_cx) while the parent is parked on the scope | It compiles and locks as the child, because the acting fiber is sched.current (Rule 13). The parent is never enqueued as a waiter. |

**Miri and sanitizers.**

- Miri cannot run naked asm. Keep the wait list, ready queue, timer wheel and `post_state` testable without a context switch, and run `cargo +nightly miri test --lib` on those modules.
- Run ASan and TSan on nightly. Fiber stacks need the sanitizer fiber-switch hooks, so keep `detail/sanitizer.rs` as in the C++ (`__sanitizer_start_switch_fiber`, `__tsan_switch_to_fiber`).
- Every integration test, example and bench begins with `#![forbid(unsafe_code)]`.

**Benchmarks.** Follow `AGENTS.md`: compare tinyio builds against each other, interleaved round by round and paired per round, with a fresh data file for disk runs.

## Build stages

Build in nine small stages, each ending in a green verification command. The first plan's Task 4 held buffer pool, loop, I/O and runtime in one step; here it is stages 2 to 5.

**Prompt for each stage:** section 2 (all rules), section 4 (the API sketch), the rows of sections 3 and 7 the stage touches, the subsection of section 6 it implements (all of section 6 for stages 2 to 5), the stage row below, and the C++ file the row names as reference. Nothing else. Tell the model to output only the listed files, run the verification command, fix until green, and stop.

| Stage | Build | Done when |
| --- | --- | --- |
| 0. Skeleton | `Cargo.toml` (edition 2024, `rust-version = "1.88"`, deps `libc`, `io-uring`, `crossbeam-queue`; optional `tracing` feature), crate lints from Rule 0, a CI script that fails on `pub unsafe` | `cargo clippy --all-targets -- -D warnings` |
| 1. Fibers | `detail/context.rs` (port of C++ detail/context.cc for x86\_64 and aarch64, with FP control state and unwind CFI), `stack.rs`, `fiber.rs`, `sched.rs`, a ring-less `Io` with `spawn`, `yield_now`, `run_until_done` | fiber unit tests (layer 2), ready-queue double-push test, `cargo +nightly miri test --lib` on the queue and wait list, context-switch bench; a panic in a fiber aborts with a readable message (subprocess test) |
| 2. I/O core | `ring.rs`, `cx.rs` `syscall`, `UserData`, `inflight`, the tick with its quiesce check, `read`/`write`/`open`/`close`, `Drop for Io` that aborts if work is left, the cancel-and-drain routine (run calls it in stage 4; tests use an internal hook), and sleep as a Timeout SQE. No wheel timeouts, no `Remote`. | layer 3 file and pipe tests; the cancel-and-drain hook with an op in flight ends with nothing in flight and does not hang; dropping `Io` after `run_until_done` is clean |
| 3. Sync | `sync/waiters.rs`, `Mutex<T>`, `Semaphore`, `Latch`, `Baton`, `scope`, `WaitKind` abort report, the death-test harness | layer 4 death tests; mutex handoff test; the `compile_fail` cases for `Mutex` and `scope` |
| 4. Remote and shutdown | `remote.rs`, `post`, `stop`, eventfd wake, closed bit, `run`, the full shutdown sequence | 8-thread post test; stop cancels waiting and ready fibers; no post succeeds after close |
| 5. Timeouts | `timer.rs`, `Cx::timeout`, `process_expired_timers`, the `ETIMEDOUT` mapping | wheel unit tests; the late-cancel test from section 8; a timed op interrupted by stop returns ECANCELED, not ETIMEDOUT |
| 6. Net | `net/` listener and stream, socket options, `accept`/`connect`/`poll`, the `with(cx)` `Read`/`Write` adapter, examples 01–05 | examples run under `#![forbid(unsafe_code)]`; echo test passes |
| 7. Fixed buffers (optional) | `buffers.rs`: pools registered with the ring, `FixedBuf<'io>` leases, `take` (EAGAIN) and `acquire` (parks) | buffer-pool tests; disk benches |
| 8. Hardening | aarch64 on real hardware, sanitizer hooks, ASan/TSan/Miri in CI, A/B benches against the C++ baseline | CI green on both architectures |

### Model-driven workflow

**Per stage, the driver model gets:** sections 2 and 4, the rows of sections 3 and 7 the stage touches, the matching part of section 6, the stage row, and the C++ file the row names. It outputs only the listed files, runs the verification command, fixes until green, and stops.

**Gate before review.** The stage goes to review only when all of these hold:

1. The stage's verification command is green.
2. `grep -R "pub unsafe" src` is empty.
3. Every `unsafe` block has a `// SAFETY:` comment naming an invariant from section 3.
4. Only the files in the stage row changed, and the assembly differs from the C++ port only where the stage says so.
5. No `panic!`, `RefCell`, `catch_unwind` or `thread_local!` in scheduler or ring state (Rules 5 and 8).

**Review by a stronger model** (stages 1 to 4 always; later stages only if the diff touches `unsafe`). Prompt:

> You are reviewing one stage of a Rust runtime built from the attached plan. Input: the diff, sections 2, 3 and 7 of the plan, and the C++ reference file named in the stage row. For each `unsafe` block, answer: (1) is its SAFETY claim true? (2) can any sequence of safe calls break it? (3) does a reference created there live across a context switch? Then check the diff against Rules 12 and 13 and the invariants the stage touches. Output a ranked list of findings with file, line, failure scenario and fix. Do not rewrite the code.

**When to change driver.** Move the stage, and later stages that touch `unsafe`, to a stronger model if any of these happens:

- The reviewer finds a soundness bug in an `unsafe` block.
- The driver weakens a test or a rule to get green.
- The driver invents assembly instead of porting `context.cc`.
- The driver starts the next stage.

If the review finds only style issues, keep the same driver for the next stage.

## Open decisions

These need an answer before or during the stage named. I recommend the first option in each.

1. **How primitives reach the scheduler (stage 3).** Recommended: each primitive holds an `Rc<Sched>` handle. Ring teardown stays in `Io::drop`, never in `Sched::drop`, so a leaked `Rc<Mutex>` keeps memory alive but not the ring. The alternative is a lifetime-bound `Mutex<'a, T>`, which cannot be shared between `'static` fibers.
2. **Join handles and per-fiber cancel (after v1).** `spawn` returns only a `FiberId` today. A `JoinHandle<T>` needs result storage and a park on the join; per-fiber cancel must keep Rule 3, so it is an `ASYNC_CANCEL` and the fiber still resumes on its CQE.
3. **Closure larger than half a stack (stage 1).** Recommended: return `ENOMEM`, as the C++ does. Alternative: box it, which adds an allocation to spawn.
4. **Allocation per `post` (stage 4).** `Box<dyn FnOnce>` costs one allocation. Measure with the dispatch bench before adding small-buffer storage.
5. **A worker-pool helper (stage 6).** The C++ has none; each example builds its own threads. Because `Io` is `!Send`, a helper must build each `Io` inside its worker thread. Recommended: keep it in `examples/` and out of the library.
6. **Fixed buffers in v1 (stage 7).** Only the buffer-pool tests use them; no example or bench does. Recommended: build them after the net stage, as `FixedBuf<'io>` so a lease cannot outlive the runtime.
7. **aarch64 branch protection (stage 8).** Demoted: arm builds have never failed for this project, so do nothing unless one does. If PAC or BTI ever breaks the switch, add a `build.rs` check of `RUSTFLAGS` then.
8. **Panic policy.** Abort is the default. Decide whether a user ever needs an opt-in catch at the fiber root, and if so what happens to the payload of a detached task.
9. **Binding a `Mutex` to the scheduler (stage 3).** Recommended: `Mutex::new(&impl Attach, v)` with a sealed `Attach` implemented for `Io` and `Cx`, so shared state can be built on the owner thread before any fiber runs. The alternative is to bind lazily on the first `lock`, which hides a runtime check inside a safe call.

## Appendix: sample user code

An echo server written against the API in section 4. It compiles against the API sketch under `#![forbid(unsafe_code)]`. Use it as the target shape for the examples in stage 6 and as a smoke test of Rule 0: if a change makes this program need `unsafe`, the change is wrong.

```
#![forbid(unsafe_code)]
use tinyio::*;
use std::{net::TcpListener, rc::Rc, time::Duration};

fn main() -> std::io::Result<()> {
    let mut io = Io::new(Options::default())?;
    let remote = io.remote();

    // Shared state, built on the owner thread before any fiber runs.
    let hits = Rc::new(Mutex::new(&io, 0u64));

    let listener = TcpListener::bind("127.0.0.1:7000")?;
    let h = hits.clone();
    io.spawn(move |cx| {
        loop {
            let conn = match cx.accept(&listener) {
                Ok(c) => c,
                Err(e) if is_cancelled(&e) => return,   // shutting down
                Err(_) => continue,
            };
            let h = h.clone();
            cx.spawn(move |cx| {                         // owns conn and h; cannot capture the outer cx
                *h.lock(cx) += 1;
                let mut buf = [0u8; 4096];
                loop {
                    match cx.timeout(Duration::from_secs(5)).read(&conn, &mut buf, None) {
                        Ok(0) | Err(_) => break,
                        Ok(n) => if cx.write(&conn, &buf[..n], None).is_err() { break },
                    }
                }
            }).ok();
        }
    })?;

    std::thread::spawn(move || {                         // Remote is the only Send handle
        std::thread::sleep(Duration::from_secs(60));
        remote.stop();
    });

    io.run();   // after stop: cancel, drain, no fiber left behind
    Ok(())
}
```

- **Blocking style.** No futures and no `async`. Each `cx.read` parks the fiber until its completion arrives.
- **Timeouts** are a view over the same methods: `cx.timeout(d).read(..)`.
- **Shutdown.** Fibers see `ECANCELED` at their next I/O call; `is_cancelled` saves matching `raw_os_error()`.
- **`Rc`, not `Arc`.** Fibers stay on their thread, so `Rc` and `Mutex` need no atomics, and the compiler rejects sending them to another thread.
- **Rejected at compile time:** capturing the outer `cx` in a spawned fiber, borrowing locals in a plain `spawn`, posting an `Rc` through `Remote`.
- **Scoped fibers** borrow locals and are joined before `scope` returns:

```
let mut total = 0;
scope(cx, |s| {
    s.spawn(|cx| { total += cx.read(&a, &mut b1, None).unwrap_or(0); }).ok();
    s.spawn(|cx| { /* other work */ }).ok();
});   // returns only after both children finish
```

- **One `Io` per worker thread.** `Io` is `!Send`, so a thread-per-core server builds each `Io` inside its worker thread (open decision 5 keeps that helper in `examples/`).
