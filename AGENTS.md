# tinyio — agent working notes

## Build

The build machinery mirrors kio's: one preset per sanitizer profile, one make
command per task.

| Command | What it does |
|---------|--------------|
| `make dev` | configure + build + test the debug preset |
| `make asan` / `make tsan` | the same with AddressSanitizer / ThreadSanitizer |
| `make release` | `RelWithDebInfo`, mimalloc, benchmark server — the only build you measure |
| `make check` | asan + tsan, i.e. what CI runs before a merge |
| `make t T=<pattern>` | run the matching tests in the dev build |
| `make bench` | build the release benchmark server only |
| `make fmt` / `make fmt-check` | clang-format (v20, the version the toolchain image pins) |

- Presets live in `CMakePresets.json`; the build tree is `build/<preset>`.
- The library, the tests and the examples all compile with
  `-Wall -Wextra -Wconversion -Wpedantic -Wshadow -Wsign-conversion -Werror`
  (`cmake/TinyioWarnings.cmake`). New code has to satisfy that set.
- tinyio is Linux-only (liburing, and x86_64 / aarch64 only: the fiber
  context switch is hand-written assembly in `src/tinyio/detail/context.cc`);
  verify on a Linux box, not on macOS.
- Every dependency is declared in `cmake/TinyioDependencies.cmake` and
  nowhere else. Fetched: liburing, **YLT** (yalantinglibs) for logging and
  serialization, googletest, and mimalloc when enabled. From the system:
  gflags, for the examples only. YLT is header-only here and only the easylog
  and struct_pack modules are reachable; every optional YLT backend is off
  through the `YLT_ENABLE_*` switches.
- The pinned liburing is downloaded and configured during CMake configure, so
  `<liburing.h>` resolves for an IDE before the first build.
- One library target, `tinyio::tinyio`: static by default, shared with
  `TINYIO_BUILD_SHARED=ON`. Its headers are a `FILE_SET HEADERS` in
  `src/tinyio/CMakeLists.txt`, so a new header goes on that list.

## Layout

`src/tinyio/` holds the umbrella `tinyio.hpp` and the leaf headers every
other one leans on (`error.hpp`, `check.hpp`, `logging.hpp`). Below it:

- `runtime/` — the `IO` class, the fiber synchronization primitives in
  `fiber_sync.hpp`, and the fixed buffer pools, which are owned by and
  registered with one `IO`.
- `net/` — sockets and descriptors: `SocketAddress`, `UniqueFd`,
  `TcpListener`. It depends on nothing but `error.hpp`.
- `detail/` — the mechanism: context switch, stacks, fibers, queues, the
  timer wheel. Not API.

`src/tinyio/runtime/runtime.{hpp,cc}` is still **one class**, spread over four
translation units: `runtime.cc` (construction, activation, the run loop,
shutdown), `scheduler.cc` (fibers: spawn, resume, suspend, destroy),
`event_loop.cc` (the tick, submit/wait, reap, timers, the eventfd wakeup)
and `io_ops.cc` (the thin `Read`/`Write`/`Open`/… wrappers). Each folder
holds its headers next to their implementations.

Every project header is included by its rooted path, `"tinyio/error.hpp"` or
`"tinyio/detail/fiber.hpp"`, never relative to the including file. The source
tree under `src/` and the installed tree under `include/` have the same shape,
so the same include line resolves in both.

clang-format sorts the project's own header to the top only in the file it
belongs to — `runtime.cc` keeps `"tinyio/runtime/runtime.hpp"` first, because
the header matches its name. In every other file the project include sorts
into the quoted block at the end, which is why the four runtime translation
units begin differently.

`docs/core-reading-guide.md` is the reading order, and the invariants it
lists are the specification.

## Installing

`cmake --install build/<preset> --prefix <dir>` produces:

- `include/tinyio/` — the public headers, in the same `runtime/`, `net/` and
  `detail/` folders they have in `src/`, plus the generated `version.h`.
  `tinyio.hpp` is the umbrella and the only include a consumer needs; the
  individual headers moved when the tree was split into folders, so
  `<tinyio/runtime.hpp>` is now `<tinyio/runtime/runtime.hpp>`.
- `include/ylt/` — YLT's own headers, because the public headers include
  `<ylt/easylog.hpp>`. YLT's thirdparty and standalone trees (asio, frozen,
  cinatra, iguana) are not installed.
- `lib/libtinyio.{a,so}` and `lib/cmake/tinyio/` — the library and its CMake
  package.

A consumer needs nothing but the prefix:

```cmake
find_package(tinyio REQUIRED)
target_link_libraries(app PRIVATE tinyio::tinyio)
```

The package pulls Threads and liburing (>= 2.9, from the system, through
pkg-config) in with `find_dependency()`. Note that it asks for a **system**
liburing, not the pinned 2.9 the build tree compiles against: the pinned one
lives in the build tree and cannot be relocated. The shared flavour exports
every symbol — tinyio has no export macros on its public API yet, so hidden
visibility would leave the `.so` with nothing to link against.

## Errors and logging

tinyio uses kio's error handling and YLT's easylog for logging.

### Errors

`src/tinyio/error.hpp` carries kio's `Error`: a `std::error_code` plus the
operation that failed, wrapped in `std::expected` as `Result<T>`.

```cpp
auto listener = TcpListener::Bind(8080);
if (!listener) {
  ELOGFMT(ERROR, "bind failed: {}", listener.error().Message());
  return std::unexpected(listener.error());
}
```

kio's macros are how an error propagates:

| Macro | What it does |
|-------|--------------|
| `TRY(var, expr)` | binds the value of a `Result<T>`, returns the error if there is none |
| `TRY_VOID(expr)` | the same for a `Result<void>` |
| `TRY_LOG_ERR(var, expr, fmt, ...)` | like `TRY`, but logs the failure first |
| `TRY_LOG_ERR_VOID(expr, fmt, ...)` | like `TRY_VOID`, but logs the failure first |

`TRY_LOG_ERR` earns its extra characters: the log line points at the exact
source location that failed.

### Logging

Log with easylog's macros directly — `ELOGFMT(INFO, "… {}", value)` — there is
no tinyio logging wrapper. `ELOGFMT` is the format style, `ELOG`/`ELOGV` the
stream style. Use `ELOGFMT` when the message needs format specs (`{:<24}`,
`{:>6.1f}`) and the stream style otherwise.

Three things are worth knowing before touching a log call:

- **easylog has to be initialized.** `src/tinyio/logging.hpp` does it once,
  before `main`, with INFO and above on the console, no log file and
  synchronous writes; every public header that pulls in easylog includes it,
  so logging works out of the box. A program that wants other settings calls
  `easylog::init_log()` in `main`, which replaces them. Without an
  `init_log()` call easylog silently drops every record: its default logger
  never starts the writer thread.
- **The format backend is pinned to `std::format`.** easylog would use the
  host's fmt whenever `<fmt/format.h>` is installed, and the fmt that ships
  with Ubuntu 25.04 (10.1.1) does not compile with clang 21 in the
  header-only mode easylog forces. `src/tinyio/logging.hpp` keeps fmt out of
  the include and points `ELOGFMT` at `std::format`, so **always include
  `tinyio/logging.hpp`, never `<ylt/easylog.hpp>` directly**: a direct include
  that comes first brings the host's fmt back.
- **YLT is pinned to the `0.6.1` release tag**, not to a branch. 0.6.1 has the
  easylog fix for a lost wakeup in `appender::stop()`: the old code set the
  stop flag while holding the file mutex instead of the queue mutex the writer
  thread waits on, so the notification could be dropped and `~appender()`
  would block forever in `join()` — any tinyio program that logged and exited
  could hang at exit. The same release dropped the `start_thread()` call from
  easylog's default logger, which is why the initialization above matters.

### Stack traces

easylog prints a stack trace on a fatal record, from `std::stacktrace` when
the standard library can link it and from YLT's own backtrace helper
otherwise. `cmake/TinyioToolchain.cmake` probes which one works at configure
time: some toolchains ship the `<stacktrace>` header without the matching
runtime support (GCC 15 headers on a GCC 16 runtime), and then every
translation unit that reaches easylog's fatal path fails to link.

When the probe fails — for example GCC 15 headers on a GCC 16 runtime, where
libstdc++ exports no stacktrace symbols at all — the fallback is used and the
trace shows bare addresses: YLT's `b_stacktrace` zeroes the module base before
handing the address to `addr2line`, so nothing resolves. On a box whose
headers and runtime match, `std::stacktrace` is used instead and the trace
carries function names. The executables are linked with `-rdynamic` so the
main binary's symbols are in the dynamic symbol table.

## Performance work: tinyio is the baseline

**tinyio is the subject and the reference point of this project's performance
work. Compare tinyio against itself — never tinyio against kio, coro_io, or any
other library.** A "before/after" pair of tinyio builds is the only comparison
worth running; a cross-library table is not, and does not answer the question
being asked.

Concretely, when measuring a change to tinyio:

- Build two tinyio variants (baseline vs candidate) and A/B **only** those.
- The baseline is the current tree, including the kio-style loop
  (resume, then submit, then reap; the only loop there is).
- kio, coro_io and fio stay as **device/hardware controls**, not competitors:
  fio for the raw NVMe ceiling, the others only as a sanity reference if a
  number needs an outside yardstick. They are never the "other arm" of the
  experiment.
- Interleave baseline and candidate round-by-round, never all of one arm
  then all of the other, and compare arms paired per round rather than by
  un-paired medians over rounds.
- Use a **pre-created fixed data file** for disk runs, recreated immediately
  before the run: NVMe SLC caching makes a stale file read about twice as
  slow, which silently invalidates every absolute number.

## Fixed buffers

tinyio's fixed buffer facility is kio's, ported over: a pool of fixed-size
slots carved out of one huge-page backed mapping and registered with the ring,
so a read or a write can name the slot instead of an address and the kernel
does not have to copy through a bounce buffer.

One runtime, a handful of pools: every entry in `IoOptions::buffer_pools` is
one **size class** with its own slot pool (the multiclass shape — a buffer is
asked for by slot size, and the runtime resolves the size class). When the
option is non-empty, the runtime maps each pool and registers it with the ring
at construction. There is no public registration API.

```cpp
IoOptions opts;
opts.buffer_pools = {{.slot_size = 4096, .slots = 256}};
IO io(0, opts);

// Inside a fiber
TRY(buf, io.TakeFixedBuffer(4096));           // non-blocking, EAGAIN when that class is empty
TRY_VOID(io.AcquireFixedBuffer(4096));        // parks the fiber instead
TRY_VOID(io.ReadFixed(fd, buf, buf.Size()));  // count discarded here
// the slot goes back to its pool when the lease is destroyed
```

Invariants worth knowing before touching this code:

- **Slot size is a power of two >= 4096 that divides 1 GiB.** io_uring caps a
  registered iovec at 1 GiB, so the mapping is registered as consecutive 1 GiB
  chunks and 1 GiB has to be an exact multiple of the slot size -- otherwise a
  slot would straddle two registered entries and no single `buf_index` would
  cover it.
- **One iovec per chunk, not per slot.** The `buf_index` is computed from the
  pointer (`first_index_ + ((ptr - base_) >> 30)`), so the kernel's registered
  buffer table holds a handful of entries instead of one per slot. That table
  is much smaller than the number of slots a pool has.
- **The mapping is 2 MiB-aligned and `MADV_HUGEPAGE`d**, base *and* length:
  fewer TLB misses during DMA.
- **Registered pages are pinned.** Without `CAP_IPC_LOCK` they count against
  `RLIMIT_MEMLOCK` (`ulimit -l`); past it the constructor throws ENOMEM and
  logs the hint.
- **A released slot is handed to the oldest waiter**, not put back on the free
  list, so a new arrival cannot steal the slot a woken fiber is about to
  receive. Waiters are served strictly FIFO. A slot handed over is the
  waiter's even if the pool closes before the waiter runs.
- **The runtime never closes its pools on shutdown** (kio's model). A fiber
  parked in `AcquireFixedBuffer()` is a sync park like any other: once the
  runtime has quiesced nothing can release a slot to it, so
  `AbortIfParkedFibers()` aborts. A leaked lease is a bug, not a shutdown
  path.
- **The abort names the primitive.** Every parked fiber records what it is
  waiting on (`WaitKind`: `Mutex`, `Semaphore`, `Latch`, `TaskGroup`,
  `FixedBufferPool`), so the message is "1 on Latch (fiber 3), 1 on
  FixedBufferPool (fiber 4)" instead of a count plus guesses. The reason lives in the
  parked fiber, so there is no counter to keep — a coroutine runtime needs
  one, a stackful runtime does not.
- **`ReadFixed`/`WriteFixed` only take this runtime's slots.** The kernel
  resolves `buf_index` against this ring's table, so a slot from any other
  pool is a CHECK failure. A buffer offset outside the slot or a zero length
  is EINVAL; a length past the slot is clamped.
- **The pool is detached before the fibers are destroyed.** `Detach()` drops
  the waiters and the owner link, so the pool's destructor wakes nobody.
- **Leases must not outlive the runtime.** The runtime owns the pool; a
  lease destroyed after it returns its slot to freed memory.

The reasoning behind all of this is in `docs/buffer-pool.md`.

## Known knobs

- The loop is kio's pipeline: resume, then submit, then reap. (The older
  submit-first ordering was removed after the A/B: ~3% less throughput,
  ~4× worse p99.) Resume-first **requires** the quiesce check before the
  blocking submit, and that check must cover every drain
  (`DriveToQuiescence()`) — omitting it hangs
  `ShutdownCancellationTest.StopTokenCancelsWaiting`.
- Ring flags: tinyio's default is
  `SINGLE_ISSUER|DEFER_TASKRUN|TASKRUN_FLAG` — note it does **not** include
  `IORING_SETUP_COOP_TASKRUN`, unlike kio's default. The kernel rejects
  `TASKRUN_FLAG` without `COOP_TASKRUN` unless `DEFER_TASKRUN` is also set.
- **Keep `IORING_ENTER_GETEVENTS` on the non-blocking `SubmitOrWait()` path
  whenever `DEFER_TASKRUN` is set.** Even with `IORING_SETUP_TASKRUN_FLAG`
  enabled, calling `io_uring_submit()` instead of
  `io_uring_submit_and_get_events()` when `runnable` is true omits
  `IORING_ENTER_GETEVENTS` whenever `IORING_SQ_TASKRUN` was not yet visible in
  userspace before the syscall. The kernel then submits the batch of SQEs
  without flushing `task_work` completions generated during
  `ResumeReadyFibers()` / `io_submit_sqes()`, delaying those completions by a
  full tick and regressing HTTP `p99` by ~8–11 % in every fast-tier sample.
- **The kernel floor is 6.1**, and it is not negotiable: `DEFER_TASKRUN` is
  in the default flags (6.1) and the shutdown path cancels every request
  with `IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY` (5.19). There is
  no fallback for an older kernel — raise the documented minimum instead.
- **The submit path survives `EBUSY`/`EAGAIN`.** The kernel could not flush
  the completion queue; the documented recovery is to reap and submit again,
  which `SubmitSqes()` does, and the blocking wait in `SubmitOrWait()` reaps
  and returns to the loop. Reaping only marks fibers ready, so it is safe
  from inside a fiber's `GetSqe()` too. A tick that failed here used to abandon a
  shutdown drain with operations still in flight, and the ring was then torn
  down under them — a kernel use-after-free.
- **A drain never gives up on a tick error.** `CancelAndDrain()` and
  `RunUntilDone()` retry and only abort after `k_max_tick_errors`
  *consecutive* failures. Returning early would leave operations in flight;
  the ring teardown would then free the `Operation` objects the kernel still
  points at.
- Always run the test suite (`ctest`) against a change to the event loop;
  the shutdown/cancellation paths are where loop reordering breaks.
