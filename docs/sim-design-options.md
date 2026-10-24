# Simulation for tinyio — an options evaluation

**Status**: Draft. Options evaluation, no design chosen.
**Date**: 2026-10-24
**Supersedes**: nothing. `docs/rfc-002-sim-runtime.md` (SimRing) is *not* carried
forward: its conclusion — emulate the io_uring ABI in user space — is one of
the options evaluated below, and it does not win. The parts of RFC-002 that
are factually wrong about the current code are corrected in §2 below.

---

## 0. Verdict

**Build a simulator behind a neutral op/driver seam (Option B), and keep the
existing real-kernel suite as the authority on the kernel-facing layer.**

The seam is worth building because a simulator buys three things the real
kernel cannot give: virtual time, deterministic completion order, and a
deadlock detector that reports *which* fibers are stuck instead of hanging CI
until the timeout.

But three findings, all of them from reading the current code rather than
RFC-002, change what the simulator should be:

1. **No simulator makes tinyio run on macOS.** The fiber context switch is
   hand-written SysV/AAPCS64 assembly with ELF-only directives
   (`src/tinyio/detail/context.cc:44-158`), and the cross-thread wakeup is
   `eventfd` (`src/tinyio/runtime/runtime.cc:146`). Both sit *outside* the I/O
   seam. The honest portability target is "a Linux container without
   io_uring support", not "a MacBook". This kills the RFC's motivation #4 as
   stated, and it is the single biggest reason not to emulate the ring ABI.
2. **Timeouts are already in user space.** RFC-002 §7 says "all timer-dependent
   behavior relies on `IORING_OP_TIMEOUT`". It does not: the timer wheel
   implements every I/O timeout (`src/tinyio/detail/timer_wheel.hpp`), and the
   kernel is touched only at expiry, to issue one `ASYNC_CANCEL`. The wheel
   already takes `now` as a parameter — only the *source* of `now` is
   hard-coded. Virtual time is a one-function seam, not a design pillar.
3. **tinyio's completion protocol is already io_uring-independent.** The
   runtime hands the kernel `&fiber->operation` as `user_data` and reads back
   an `int` result (`src/tinyio/detail/fiber.hpp:82-99`,
   `src/tinyio/runtime/runtime.hpp:506-536`). A simulator only has to produce
   `(user_data, result)` pairs; it does not have to reproduce the SQE layout to
   be functionally correct.

The rest of this document develops the options, compares them, and sketches
the recommended shape.

---

## 1. What the simulator has to reproduce

The whole kernel-facing surface of tinyio is small enough to enumerate. This
is the checklist any simulator must satisfy.

| Surface | Where | Notes |
|---|---|---|
| `io_uring_queue_init_params` | `runtime.cc:141` | ring creation, flags |
| `io_uring_register_buffers` | `runtime.cc:158` | fixed-buffer pool table |
| `io_uring_register(ENABLE_RINGS)` | `runtime.cc:294` | activation |
| `io_uring_get_sqe` | `event_loop.cc:219,226` | SQ slot acquisition |
| `io_uring_submit` / `_and_get_events` / `_and_wait_timeout` | `event_loop.cc:127,152,264,291` | submit + blocking wait |
| `io_uring_get_events` | `event_loop.cc:173` | CQ overflow reaping |
| `io_uring_for_each_cqe` / `cq_advance` / `cqe_get_data64` | `event_loop.cc:183-206` | completion drain |
| 17 `io_uring_prep_*` opcodes | `io_ops.cc:39-231` | read, write, read_fixed, write_fixed, readv, writev, fsync, openat, fallocate, ftruncate, accept, connect, poll_add, unlinkat, renameat, timeout, close |
| `io_uring_prep_cancel64` | `event_loop.cc:199,253` | shutdown cancel + timeout cancel |
| eventfd read + `::write` | `runtime.cc:146`, `event_loop.cc:282,289` | cross-thread wakeup |
| `clock_gettime(CLOCK_MONOTONIC)` | `timer_wheel.hpp:13` | the only clock read |

Roughly 300 lines of runtime code stand between tinyio and the kernel. That is
the entire attack surface for a simulator, and it is small enough that the
option space is worth evaluating honestly rather than defaulting to the first
idea.

### Invariants the simulator must not break

From `docs/core-reading-guide.md`, the ones a simulator can plausibly break:

- **Invariant 3** — `inflight_ops_` accounting: incremented at submit,
  decremented exactly once per completion. The eventfd read is deliberately
  *not* counted. A simulator that counts its wake-read differently will make
  the runtime quiesce early or never.
- **Invariant 4** — `user_data` has exactly three meanings: `k_wakeup_sentinel`
  (the eventfd read), `k_fiber_cancel_sentinel` (a cancel SQE), and anything
  else (an `Operation*` embedded in the submitting fiber). The simulator
  receives these verbatim and must route them the same way.
- **Invariant 7** — the timeout cancel is submitted *immediately*, so that it
  cannot match a successor op that reuses the same `&Operation` address. A
  simulator must model cancel matching by op identity, not by address.
- **Invariant 9** — quiesce means "nothing ready, nothing in flight". The
  quiesce check in `Tick()` (`event_loop.cc:40-43`) is the subtle line in the
  runtime; a simulator that reports completions late or early moves it.

---

## 2. Findings that constrain any design

**F1 — The kernel boundary is thin, but not as thin as RFC-002 assumes.**
RFC-002's §12 open question 1 asks only about allocating ring memory. The
fixed-buffer pool registers one iovec per ≤1 GiB chunk with the ring and the
kernel resolves `buf_index` against that table (`runtime.cc:155-171`,
`io_ops.cc:87-91`). A simulator at the SQE level must implement that second
kernel interface — buffer registration and `buf_index` resolution — on top of
the opcode set. RFC-002 never mentions it.

**F2 — The SQE/CQE structs are Linux UAPI, and they are not portable.**
`liburing/io_uring.h` includes `<linux/fs.h>`, `<linux/types.h>` and
`<linux/time_types.h>`. Any design that keeps `io_uring_sqe` in the sim's
signature ties the sim to Linux headers; the sim becomes compilable only
where those headers exist.

**F3 — The timer wheel is already time-injected.** `TimerWheel::Arm` takes an
absolute deadline; `Expire(now_ns)` and `NsUntilNext(now_ns)` take `now` as
parameters. The only thing that is hard-coded is the *source* of `now`
(`MonotonicNowNs()`, `timer_wheel.hpp:13`), read at four call sites. Virtual
time is one function and four callers.

**F4 — The wakeup path can stay exactly as it is.** `Wake()` writes to a real
eventfd (`event_loop.cc:282`); the loop reads it with an ordinary `read` SQE.
The simulator can model the wake fd as one more virtual fd and leave the
`eventfd` syscall alone. (If macOS ever becomes a target, the eventfd needs
replacing anyway — see F7.)

**F5 — A simulator's fake fds can collide with real fds.** `UniqueFd`'s
destructor calls `::close(fd)` (`net/fd.hpp:14-18`), and `IO::Close(UniqueFd)`
turns the fd into a `close` SQE (`io_ops.cc:42-47`). A simulator that hands
out fd 7 while the process really has fd 7 open will close a stranger's
descriptor when the test's `UniqueFd` dies. The simulator's fd space must be
disjoint from the process's, and `close` must route to the simulator. This is
a hazard RFC-002 does not mention, and it is a *correctness* hazard, not a
style one.

**F6 — A simulator can detect deadlocks that today hang.** Today, a fiber
parked on an I/O that will never complete keeps `inflight_ops_ > 0`, so
`DriveToQuiescence` (`runtime.cc:215-233`) spins in the blocking submit until
CI kills it. A simulator knows when no in-flight op can ever complete and can
report it precisely. **This is the strongest single argument for building a
simulator, and RFC-002 does not mention it.**

**F7 — Nothing makes tinyio portable to macOS.** The context switch is
hand-written assembly with ELF-only directives (`.pushsection .text`,
`.globl`, `.hidden`, `.cfi_*`) and assumes the SysV/AAPCS64 ABI
(`context.cc:44-158`); macOS arm64 needs a Mach-O variant with the Darwin
ABI. The wakeup is `eventfd` (`sys/eventfd.h`). The buffer pool depends on
`mmap` + hugepage + `RLIMIT_MEMLOCK` semantics. Porting the *runtime* to
macOS is a separate project from building a simulator, and no option below
includes it.

**F8 — RFC-002 contradicts itself about the seam.** §4 and §5.1 insist on "a
direct boolean branch ... eliminates the need for virtual dispatch entirely",
while §5.2 says "`SimRing` implements `RingBackend`" — an interface *is*
virtual dispatch, and `RingBackend` is never defined nor listed in §8. One
of the two has to go. (For the record: the branch is right. The cost
difference is one predictable indirect call per I/O op against a syscall that
costs 1–3 µs — under 0.05%. The RFC's performance objection to the
interface-based option is a false economy, and it should not drive the
design.)

---

## 3. The question that picks the seam

Different questions want different simulators, and conflating them is what
produced RFC-002.

- **"Is my handler correct under timeouts, resets and short writes?"** →
  wants virtual time, deterministic ordering, and faults at the *operation*
  boundary. Seam level: operation.
- **"Is my runtime correct against the kernel's adversarial behavior?"** →
  wants the real kernel: CQ overflow, `EBUSY` on submit, cancel races, fd
  lifetime, the drain path. A simulator *removes* the thing under test.
  Seam level: none — this is the real-kernel suite, which already exists.
- **"Can I fuzz my handler with random fault schedules?"** → wants a seeded
  RNG and a reproducible schedule. Seam level: operation.

Only the first and third are simulator problems. The second is what the
existing `tests/tinyio/` suite already covers with a real ring. Choosing the
seam level without separating these is how you end up emulating a ring to test
a handler.

---

## 4. The options

### Option A — SQE-level emulation ("SimRing", RFC-002's design)

The simulator consumes real `io_uring_sqe` structs and produces real
`io_uring_cqe` structs. `IO` keeps its SQE-formatting code verbatim; the
liburing calls in `event_loop.cc` are replaced by calls into the simulator.

**Cost**: implement 17 opcodes plus `cancel64` and the wake-read, a virtual
fd table, the buffer-registration table with `buf_index` resolution (`F1`),
poll readiness, timeout semantics, and a virtual clock. The eventfd read
becomes a virtual op. Estimate: 1200–1500 lines of simulator plus changes at
4 call sites in the runtime.

**Wins**

- Zero change to app code, to the fiber/`Operation` protocol, and to the
  SQE-formatting layer — which is where tinyio's *own* bugs live (the
  `buf_index` math in `ReadFixed`, the `openat` path lifetime, the
  `read_fixed` clamp).
- Error codes and edge semantics are the kernel's, by construction — if the
  simulation is right.
- Fault injection at SQE granularity, including things only the ring exposes:
  CQ overflow → `EBUSY`, cancel → `ECANCELED`, timeout → `-ETIME`.

**Loses**

- Ties the simulator to Linux UAPI headers (`F2`).
- Becomes a second kernel: partial writes, EOF, `EAGAIN`, `EINTR` and poll
  readiness must all be re-derived. If they are wrong, the tests assert wrong
  behavior — and they will be wrong in the direction that hides bugs.
- The portability it appears to buy does not exist (`F7`): the simulator still
  cannot run on macOS, because the fiber machinery cannot.
- The maintenance surface is the largest of the options and it grows with
  every opcode tinyio adds.

### Option B — A neutral op descriptor and a driver seam

tinyio defines its own operation type and a driver interface; two drivers
implement it: one wraps liburing (today's code), one is the simulator.

```cpp
namespace tinyio::detail {

/// The operations tinyio can issue. One kind per public IO method family.
enum class OpCode : std::uint8_t {
  read, write, readv, writev, read_fixed, write_fixed,
  poll, sleep, fsync, open, close, accept, connect,
  fallocate, ftruncate, unlink, rename,
};

/// A completion: the 64-bit user_data and the int result, nothing else.
struct Completion {
  std::uint64_t user_data;
  int res;
};

/// The kernel boundary. The uring driver talks to io_uring; the sim driver
/// executes the ops against a virtual world.
class Driver {
 public:
  virtual ~Driver() = default;
  /// A free op slot, or nullptr when the queue is full.
  virtual Op* AcquireOp() noexcept = 0;
  /// Hand the ops to the driver. Returns the number accepted, or -errno.
  virtual int Submit(std::span<Op> ops) noexcept = 0;
  /// Wait for completions. `can_progress` is false when no pending op can
  /// ever complete: that is the deadlock signal (F6).
  virtual WaitResult Wait(unsigned min_complete, std::uint64_t timeout_ns) noexcept = 0;
  /// Drain the completions into `out`. Returns how many were written.
  virtual unsigned Reap(std::span<Completion> out) noexcept = 0;
  /// The driver's clock. The timer wheel reads its deadlines from here.
  virtual std::uint64_t NowNs() const noexcept = 0;
};

}  // namespace tinyio::detail
```

`IO` holds a `Driver&` chosen at construction. The op sites in `io_ops.cc`
fill an `Op` instead of formatting an SQE; `UringDriver::Submit` converts
`Op` → SQE. `IO::tick()` branches on the driver once per tick, and the
per-op cost is one indirect call plus a handful of stores — unmeasurable next
to the syscall it precedes.

**Wins**

- The simulator implements exactly the ops tinyio exposes — no more.
- The contract becomes *explicit and reviewable*: "what does `Write` do when
  the socket buffer is full?" stops being an emergent property of the kernel
  and becomes a line of tinyio's own code. That is the real deliverable of
  this work, more than the simulator itself.
- No Linux UAPI in the simulator → it is portable C++ that can be built and
  unit-tested anywhere, even if the runtime cannot yet run there.
- Virtual time is a natural part of the interface (`NowNs()`), not a special
  case bolted onto the timer wheel.
- Determinism is available: the driver chooses completion order, so a test
  can assert an exact interleaving that the kernel would never reproduce.

**Loses**

- Touches all 17 op sites — mechanical, but real churn, and it lands in the
  hot path.
- The `Op` → SQE adapter becomes a layer the simulator does **not** cover.
  Only the real-kernel suite covers it. This is the price of B, and it is
  what the parity test in §6 is for.
- Risk of divergence between the two drivers (the kernel's short-write
  semantics vs the simulator's) unless the contract is written down and
  tested.

### Option C — A parallel `SimIO` class (a mock, not a simulator)

A separate class with in-memory `Read`/`Write`/`Accept` and its own
scheduler.

**Wins**: fastest to build; total control over semantics; typed handles
instead of ints, so no fd-collision hazard.

**Loses**: app code must become generic (`auto& io`) or be duplicated — the
usability cliff RFC-002 §3.2 rejected. The production path is not under test:
it validates your handler, not tinyio. The fiber scheduler gets duplicated
or extracted, and both outcomes are worse than the seam in Option B. It is
worth keeping in mind as a *complement* for pure app-logic tests, but it is
not a simulator and does not replace one.

### Option D — Syscall interception (`LD_PRELOAD` / `seccomp-notify`)

Intercept `io_uring_setup`/`io_uring_enter`/`mmap`/`io_uring_register`, the
fd operations, and `clock_gettime`. tinyio is not modified at all.

**Wins**: literally zero source change; can fault-inject the benchmark
binary and third-party code too; a good chaos-testing tool.

**Loses**: fragile — glibc/musl, static linking, container seccomp policies
all get in the way. To keep liburing coherent you must emulate the ring
*memory layout* the kernel maintains, which is Option A's work with less
control and more fragility. macOS is out of scope entirely (there is no
io_uring to intercept). And the kernel's internal hrtimer is out of reach,
so `IORING_OP_TIMEOUT` cannot be virtualized — `SleepForNs` would stay real
time (the timer wheel would virtualize, since it reads `clock_gettime`).

### Option E — No simulator: a fault-injecting environment

Keep the real ring. Inject faults from outside: FUSE for files (it can
return `EIO`, `ENOSPC`, short reads on demand), a TCP proxy that can `RST`
mid-stream, `tc netem` for loss and latency.

**Wins**: maximum fidelity, and the *only* option that exercises the actual
kernel interaction — CQ overflow, `EBUSY` on submit, cancel races, fd
lifetime, the drain path. Zero runtime changes. It is also what exists today:
the suite in `tests/tinyio/` is already an Option E suite.

**Loses**: not deterministic — assertions about timing flake. Virtual time is
not available (time namespaces need privileges and only offset
`CLOCK_MONOTONIC`; the kernel's hrtimers still use real time). FUSE needs
`/dev/fuse` and privileges. Slow. Linux-only, so a macOS dev box still
cannot run it.

**This is not a replacement for a simulator — it is the control the simulator
is measured against.** It must stay in CI.

---

## 5. Comparison

| | A: SQE-level | B: op + driver seam | C: parallel class | D: interception | E: real kernel |
|---|---|---|---|---|---|
| App code changes | none | none | **templates everything** | none | none |
| Production path exercised | **yes, incl. SQE formatting** | partially (adapter untested by sim) | no | yes | **yes** |
| Virtual time | yes | yes | yes | partial | **no** |
| Deterministic ordering | yes | yes | yes | no | **no** |
| Fault injection site | SQE | operation | object | syscall | device |
| Deadlock detection | possible | **possible** | trivial | no | no (hangs) |
| Portable simulator | no (UAPI) | **yes** | yes | no | n/a |
| Runtime code churn | 4 call sites | 17 op sites + runtime | new runtime | none | none |
| Simulator size (est.) | 1200–1500 LOC | 600–900 LOC | 400–600 LOC | 800–1200 LOC | n/a |
| Catches kernel races | no | no | no | partly | **yes** |
| Reaches macOS CI | no | no | no | no | no |

Read the last row honestly: **no row reaches macOS**, because `F7` is
independent of the simulator design. The portable target is a Linux container
without io_uring — which Option B delivers, and which Option A also delivers
(Linux-only sim in a container). Portability is therefore *not* the
discriminator between A and B; it is a tie.

---

## 6. Recommendation

**Option B, with the Option E suite kept in CI as the kernel-facing
authority, plus a driver-parity test to recover what the simulator does not
cover.**

The reasoning, in one line each:

- A simulator earns its keep by giving you virtual time, deterministic order,
  and a deadlock detector (F6). None of those require emulating the ring ABI.
- Option A's unique benefit is that it also tests the SQE-formatting layer.
  That layer is already covered by the real-kernel suite, which exists and
  passes — so A's marginal benefit does not justify a second kernel.
- Option B's cost is that the `Op` → SQE adapter is covered only by the
  real-kernel suite. The parity test closes that gap: run the same scenario
  against both drivers and assert identical *outcomes* (bytes transferred,
  error codes). Not interleavings — the kernel's completion order is
  genuinely nondeterministic, and asserting on it would make the parity test
  flaky. The parity test runs in CI, so the adapter is exercised on every
  run, which is exactly what Option A would have bought.

### The shape of the recommended design

```cpp
namespace tinyio::sim {

/// The virtual world: virtual fds, a virtual clock, and a fault table.
class World {
 public:
  /// A connected pair of virtual sockets.
  std::pair<int, int> MakePipe();
  /// A virtual file backed by an in-memory buffer.
  int MakeFile(std::size_t capacity);

  /// Advance the virtual clock, firing every deadline it passes.
  void Advance(std::chrono::nanoseconds delta);
  /// Advance to the next pending deadline. Returns false if there is none.
  bool AdvanceToNextDeadline();

  /// Faults, per virtual fd.
  void FailAfterBytes(int fd, std::size_t bytes, int err);
  void FailOnOp(int fd, std::size_t nth, int err);
  void CapTransfer(int fd, std::size_t bytes);   // short writes
  void Disconnect(int fd);                       // ECONNRESET

  /// True when no in-flight op can ever complete (a deadlock).
  bool Deadlocked() const;
};

}  // namespace tinyio::sim
```

Usage from a test:

```cpp
tinyio::sim::World world;
IoOptions opts;
opts.driver = world.Driver();     // a tinyio::detail::Driver*
IO io{0, opts};

auto [client, server] = world.MakePipe();
// ... spawn fibers, io.RunUntilDone() ...
world.Advance(std::chrono::seconds(30));   // instant, deterministic
```

### Requirements the design must meet

1. **The fd space is disjoint from the process's** (F5). Allocate virtual fds
   from a high range that the process never uses, never reuse them, and route
   `close` to the simulator.
2. **The wake path stays a real eventfd** (F4). The simulator models it as one
   more virtual fd; the `eventfd` syscall is untouched.
3. **Cancel matches by op identity, not by address** (invariant 7).
4. **`Wait()` distinguishes "no completions yet" from "nothing can ever
   complete"** (F6). Without this, the simulator turns today's hang into a
   100% CPU busy-spin, which is worse than the kernel's behavior. The
   runtime should then report the deadlock the way `AbortIfParkedFibers()`
   reports parked fibers: by name and count.
5. **Completion order is defined** (FIFO by submission, or by deadline for
   timed ops) so interleavings are assertable.
6. **A seeded RNG drives any randomized fault schedule**, so a fuzz failure
   is reproducible from its seed.

### Staged plan

| Stage | Change | Risk |
|---|---|---|
| 0 | Clock seam: `MonotonicNowNs()` → an injectable source | Low. The clock is read ~1–2× per tick, not per op — but it *is* in the tick path, so A/B it (tinyio vs tinyio, per `AGENTS.md`). |
| 1 | `Op` + `Driver` + `UringDriver`; no behavior change | **Highest.** The hot path changes. Measure: two tinyio builds, interleaved, median of ≥9 rounds, on srv401. |
| 2 | `SimDriver` + `World`: files, pipes, clock, faults | Medium. Pure addition. |
| 3 | Deadlock detection + parity tests against the uring driver | Medium. The parity test is what keeps stage 1 honest. |
| 4 | Seeded fault schedules / fuzzing | Low, additive. |

Stage 1 is the one that needs a performance answer, and the answer must come
from the A/B protocol in `AGENTS.md` — tinyio against tinyio, never against
another library.

---

## 7. Open questions

1. **Does the `Op` → SQE conversion cost anything measurable?** It adds a
   copy per op to a path that ends in a 1–3 µs syscall. Probably noise, but
   "probably" is not a measurement, and stage 1 is the stage where a
   regression would hide.
2. **What is the contract for `Write` on a full buffer?** Short write, or
   `EAGAIN`? The kernel picks; a simulator has to pick and write it down.
   This is the single most important sentence the project will write.
3. **Does the parity test cover enough?** It can assert outcomes, not
   interleavings. Some bugs (a lost wakeup, a double reap) are only visible
   in the ordering, and only the real kernel can produce them.
4. **Should the simulator model TCP at all?** Modeling RST and short writes
   is cheap; modeling congestion windows and reordering is not, and probably
   not worth it. The boundary needs drawing deliberately.
5. **Is `SimIO` the right name?** The RFC's name implies a parallel runtime,
   which is Option C, which is not what is being recommended. `sim::World`
   plus a `Driver` describes the thing accurately.
