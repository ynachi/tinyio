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
  verify on srv401, not on macOS.
- Every dependency is declared in `cmake/TinyioDependencies.cmake` and
  nowhere else. Fetched: liburing, **YLT** (yalantinglibs) for logging and
  serialization, google/crc32c for the WAL checksums, googletest, and mimalloc
  when enabled. From the system: gflags, for the examples only. YLT is
  header-only here and only the easylog and struct_pack modules are reachable;
  every optional YLT backend is off through the `YLT_ENABLE_*` switches.
- The pinned liburing is downloaded and configured during CMake configure, so
  `<liburing.h>` resolves for an IDE before the first build.
- One library target, `tinyio::tinyio`: static by default, shared with
  `TINYIO_BUILD_SHARED=ON`. Its headers are a `FILE_SET HEADERS` in
  `src/tinyio/CMakeLists.txt`, so a new header goes on that list.

## Layout

`src/tinyio/` holds the umbrella `tinyio.hpp` and the leaf headers every
other one leans on (`error.hpp`, `check.hpp`, `logging.hpp`). Below it:

- `runtime/` — the `IO` class, the fiber synchronization primitives in
  `fiber_sync.hpp`, and the fixed buffer pool, which is owned by and
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
- `include/crc32c/`, `lib/libcrc32c.a` and `lib/cmake/Crc32c/` — google/crc32c,
  which a static `libtinyio.a` still needs at link time.

A consumer needs nothing but the prefix:

```cmake
find_package(tinyio REQUIRED)
target_link_libraries(app PRIVATE tinyio::tinyio)
```

The package pulls Threads, Crc32c and liburing (>= 2.9, from the system,
through pkg-config) in with `find_dependency()`. Note that it asks for a **system**
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

On srv401 the probe fails — that box runs GCC 15 headers on a GCC 16 runtime,
and GCC 16's libstdc++ exports no stacktrace symbols at all — so the fallback
is used and the trace shows bare addresses: YLT's `b_stacktrace` zeroes the
module base before handing the address to `addr2line`, so nothing resolves.
On a box whose headers and runtime match, `std::stacktrace` is used instead
and the trace carries function names. The executables are linked with
`-rdynamic` so the main binary's symbols are in the dynamic symbol table.

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

### Reference numbers for the current baseline

Measured on 2026-10-25 from `4bfd719` (plus the WAL compile fix), release
preset, GCC 15.2 on srv401 (Ubuntu 26.04, kernel 7.0.0).

Disk — one thread, one shared **pre-created** 64 MiB file (recreated right
before the run, see below), 4 KiB `O_DIRECT`, random offsets, median of 12
rounds:

| QD | IOPS | mean µs | p50 µs | p99 µs | noise floor (stdev) |
|----|------|---------|--------|--------|---------------------|
| 1   | 11,881 | 80.3 | 79.6 | 90.8 | 149 IOPS (1.3 %) |
| 32  | 120,146 | 262.9 | 259.0 | 350.8 | 1,328 IOPS (1.1 %) |
| 128 | 133,460 | 947.0 | 924.2 | 1,148.3 | 463 IOPS (0.35 %) |

The stdev is the noise floor: a candidate has to beat the baseline by more
than ~1 % at QD 1/32 and ~0.5 % at QD 128 to mean anything. At QD 1 fio reads
the same file at 12,749 IOPS, so that ~7 % is what the runtime costs against
raw syscalls.

Network — 512 connections, 15 s, 4 worker threads, saturation, median of 17
interleaved samples, **fast-tier median** (the link is tri-modal, see below):

| req/s | p50 | p90 | p99 | p99.9 | cpu |
|-------|-----|-----|-----|-------|-----|
| 334,231 | 1.489 ms | 1.870 ms | 2.554 ms | 3.440 ms | 352 % |

7 of the 17 samples landed in the fast tier (328,205 – 336,397 req/s,
stdev 2,645). The other 10 sat at a 302,998 req/s median with a p99 of
9.7 – 14.0 ms: that is the box's other moods, not the runtime. Every sample
had a 100 % success rate.

## Measuring on srv401 (192.168.1.155)

- `/tmp` on that host is **tmpfs** — disk benchmarks must run from
  `/var/tmp/diskbench` (real NVMe, ext4 via LVM).
- The load generator must **never** share the box with the server: client is
  srv501 (192.168.1.177), `10.10.10.1` ↔ `10.10.10.2` over the direct 2.5 Gbit
  link.
- **srv401 cannot ssh to itself.** `ssh ynachi@192.168.1.155` *from* srv401
  fails with "Host key verification failed" — the box does not know its own
  host key. A benchmark script that runs *on* srv401 has to start and stop the
  server locally and use ssh only for the client box (srv501, which works
  fine). The Oct 2026 `ab_net.sh` script ssh'd to both boxes and therefore
  only ever worked when it was launched from a workstation, not from the box.
  A script that gets this wrong fails silently: the server never starts, the
  samples come back empty, and the whole run is wasted.
- Code lives under `/tmp/bench/` on the remote: `tinyio` (working clone),
  `tinyio_port` (clean clone of this tree used to build the benchmarks),
  `kio_v2`, `ylt`. The 2026-10-25 baseline lives in `base_4bfd719/`, with
  `base_disk.sh`, `base_net.sh`, `agg_base_disk.py` and `agg_base_net.py`
  next to it in `/tmp/bench/`. `/tmp` there is tmpfs, so the baseline binaries
  and the raw TSVs are also copied to `~/baseline_2026_10_25/` on srv401
  (`bin/`, `scripts/`, `data/`), which survives a reboot.
- **After an `rsync` to a remote build tree, touch the sources.** `rsync -a`
  preserves mtimes, so an object compiled before a header edit can look newer
  than that header and ninja will not rebuild it. The tree then mixes objects
  compiled against two different layouts of `IO`, and the fiber machinery
  crashes on a garbage `current_fiber_` — a build bookkeeping bug that looks
  exactly like a memory corruption in the hand-written context switch.
  `find <tree>/src <tree>/tests -name '*.cc' -o -name '*.hpp' | xargs touch`
  before rebuilding a tree that was synced.

### The disk box has two modes — but fio is not the detector for it

The NVMe alternates between roughly **133k** and **102k** IOPS at QD 128, and
in the slow mode the *median* request latency rises ~32% and per-read CPU
~40%. Every runtime gets caught in both.

**But fio's QD 128 number does not tell you which mode the box is in.**
Measured on 2026-10-25, same file, same CPUs, same runtime:

| | QD 1 | QD 32 | QD 128 |
|---|------|-------|--------|
| tinyio | 11,881 | 120,146 | **133,460** |
| fio, io_uring | 12,749 | 104,750 – 123,199 | 112,607 – 122,302 |
| fio, libaio | 12,721 | 100,614 – 113,336 | 96,745 – 113,670 |

fio never gets near 133k — with either engine — and its own numbers swing by
~25% between runs. On 2026-10-25 the old "fio QD 128 < 115k ⇒ slow mode"
rule fired in **10 of 12 rounds** while tinyio was at 133k in **all 12** of
them: the rule is a fio artifact, not a reading of the box. tinyio itself
landed in the slow mode in 1 round out of 12 on 2026-10-24, and in none on
2026-10-25.

So use **tinyio's own per-round spread** as the noise floor, and keep fio as a
QD 1 anchor only (at QD 1 fio is the faster one, by ~7%: that is what the
runtime costs against raw syscalls). A second tinyio build is a better device
control than fio.

The rest of the protocol stands:

1. Use a **pre-created fixed data file** (`--file PATH`), so file placement on
   the NVMe cannot vary between arms.
2. **Recreate that file immediately before the run** (e.g. `mkpattern`): the
   NVMe keeps recently written data in its SLC cache, and a file whose pages
   have aged out of it reads about **twice as slow** — QD 1 drops from ~83 µs
   to ~155 µs, which silently invalidates every absolute number. A fresh
   copy of an old file is fast again; the content does not matter, only how
   recently the file was written. Check the pre-flight QD 1 number against
   the table above before trusting a run. Recreating the file **per round**,
   at the same path, keeps the blocks where they were and the pages hot.
3. **Interleave** baseline and candidate round-by-round; never run all of A
   then all of B.
4. Report the **median over ≥9 rounds**, and state how many runs landed in
   the slow mode.
5. fio still needs `--time_based=1`, otherwise it stops after one pass over
   the file and reports nonsense.

A single run means nothing here.

### The network link is tri-modal — split before you compare

The HTTP path lands in one of three tiers, and **which tier a given sample
lands in is decided by the box, not by the code**: the tier changes from
sample to sample *and* from arm to arm inside the same sample.

| tier | req/s | p99 | tail |
|------|-------|-----|------|
| A fast | ~333,000 | ~2.6 ms | clean |
| B mid | ~324,000 | ~8–10 ms | one bad tail |
| C slow | ~303,000 | ~11–14 ms | bad tail |

A plain median over 17 samples straddles the gap differently for each arm, so
it is **not** a valid estimator: in the 2026-10 run the two arms' medians
landed on opposite sides of the gap and produced an apparent −2.5 % that was
pure mix. Bucket the samples by tier first (the largest gap in the sorted p99
values splits them cleanly: ~3.3 ms → ~7.8 ms), then compare the arms inside
each tier. Per tier the arms agree to well under 1 %.

The 2026-10-25 baseline run split on a single gap (2.78 ms → 9.67 ms):
7 samples in the fast tier, 10 in the slow one. The fast tier was tight
(328,205 – 336,397 req/s, stdev 2,645) and is the estimator; the slow tier
sat at a 302,998 req/s median with p99 between 9.7 ms and 14.0 ms. When the
run produces only a handful of fast-tier samples, say so — a fast-tier median
over 3 samples is not a baseline.

## Fixed buffers

tinyio's fixed buffer facility is kio's, ported over: a pool of fixed-size
slots carved out of one huge-page backed mapping and registered with the ring,
so a read or a write can name the slot instead of an address and the kernel
does not have to copy through a bounce buffer.

One runtime, one pool, as in kio. The pool is an option: when
`IoOptions::buffer_pool` is set, the runtime maps the pool and registers it
with the ring at construction. There is no public registration API.

```cpp
IoOptions opts;
opts.buffer_pool = BufferPoolConfig{.slot_size = 4096, .slots = 256};
IO io(0, opts);

// Inside a fiber
auto buf = TRY(io.TakeFixedBuffer());       // non-blocking, EAGAIN when empty
auto wait = io.AcquireFixedBuffer();        // parks the fiber when empty
auto n    = TRY(io.ReadFixed(fd, *buf, buf->Size()));
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
- **The runtime never closes its pool on shutdown** (kio's model). A fiber
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
