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
- `src/libs/` holds vendored third-party headers and is excluded from
  formatting, the same way kio excludes its `include/libs`.
- tinyio is Linux-only (liburing + boost::context); verify on srv401, not on
  macOS.

## Performance work: tinyio is the baseline

**tinyio is the subject and the reference point of this project's performance
work. Compare tinyio against itself — never tinyio against kio, coro_io, or any
other library.** A "before/after" pair of tinyio builds is the only comparison
worth running; a cross-library table is not, and does not answer the question
being asked.

Concretely, when measuring a change to tinyio:

- Build two tinyio variants (baseline vs candidate) and A/B **only** those.
- The baseline is the current tree, including the kio-style loop port
  (`IoOptions::loop_style = LoopStyle::kResumeFirst`, the default).
- kio, coro_io and fio stay as **device/hardware controls**, not competitors:
  fio for the raw NVMe ceiling, the others only as a sanity reference if a
  number needs an outside yardstick. They are never the "other arm" of the
  experiment.

### Reference numbers for the current baseline

Disk — one thread, one shared **pre-created** 64 MiB file, 4 KiB `O_DIRECT`,
random offsets, median of 5 rounds:

| QD | IOPS | mean µs | p50 µs | p99 µs |
|----|------|---------|--------|--------|
| 1   | 11,749 | 81.2 | 80.9 | 90.9 |
| 32  | 119,254 | 264.7 | 260.4 | 355.2 |
| 128 | 133,772 | 945.1 | 922.0 | 1,143.0 |

Network — 512 connections, 15 s, 4 worker threads, saturation, median of 17
interleaved samples:

| req/s | p50 | p90 | p99 |
|-------|-----|-----|-----|
| 333,441 | 1.486 ms | 1.871 ms | 2.501 ms |

## Measuring on srv401 (192.168.1.155)

- `/tmp` on that host is **tmpfs** — disk benchmarks must run from
  `/var/tmp/diskbench` (real NVMe, ext4 via LVM).
- The load generator must **never** share the box with the server: client is
  srv501 (192.168.1.177), `10.10.10.1` ↔ `10.10.10.2` over the direct 2.5 Gbit
  link.
- Code lives under `/tmp/bench/` on the remote: `tinyio` (working clone),
  `tinyio_port` (clean clone of this tree used to build the benchmarks),
  `kio_v2`, `ylt`.

### The disk box has two modes — always control for it

The NVMe alternates between roughly **133k** and **102k** IOPS at QD 128. This
is the device or the block layer, not the runtime: fio shows the same two modes
on the same file, and the slow mode raises the *median* request latency ~32%
and per-read CPU ~40%. Every runtime gets caught in both.

Therefore, for any disk A/B:

1. Use a **pre-created fixed data file** (`--file PATH`), so file placement on
   the NVMe cannot vary between arms.
2. **Interleave** baseline and candidate round-by-round; never run all of A
   then all of B.
3. Report the **median over ≥9 rounds**, and state how many runs landed in
   the slow mode.
4. Keep fio on the same file as a **device control** — it is how you know
   whether a shift is the box or the code.

A single run means nothing here.

## Known knobs

- `IoOptions::loop_style`: `kResumeFirst` (default, kio's pipeline — resume,
  then submit, then reap) or `kSubmitFirst` (the older ordering). The
  resume-first path **requires** the quiesce check before the blocking
  submit, and that check must cover `RunUntilDone()` *and*
  `CancelAndDrain()` — omitting the latter hangs
  `ShutdownCancellationTest.StopTokenCancelsWaiting`.
- Ring flags: tinyio's default is
  `SINGLE_ISSUER|DEFER_TASKRUN|TASKRUN_FLAG` — note it does **not** include
  `IORING_SETUP_COOP_TASKRUN`, unlike kio's default. The kernel rejects
  `TASKRUN_FLAG` without `COOP_TASKRUN` unless `DEFER_TASKRUN` is also set.
- Always run the test suite (`ctest`) against a change to the event loop;
  the shutdown/cancellation paths are where loop reordering breaks.
