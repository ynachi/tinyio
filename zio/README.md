# zio

`zio` is a benchmark prototype for a small stackful `io_uring` scheduler.

It is intentionally kept separate from the KIO library code. The goal is to
compare a fixed-pool fiber scheduler against KIO fiber without migrating KIO.

## Layout

```text
zio/
  io_context.hpp          # scheduler + TCP listener prototype
  bench/
    http_bench.cpp        # HTTP keep-alive server for oha/wrk-style load
    file_io_bench.cpp     # sequential write benchmark
    cross_thread_spawn_smoke.cpp
```

## Scheduler Shape

- One `zio::io_context` owns one `io_uring`.
- A fixed pool of `Fiber` objects is allocated at startup.
- Each fiber has a preallocated guarded stack.
- Boost.Intrusive lists/sets hold ready, free, and timer-waiting fibers.
- Fiber switching uses the public `boost::context::fiber` API with
  preallocated stacks.
- I/O APIs (`read`, `write`, `accept`, `recv`, `send`) all go through one
  `submit_wait()` path.
- `spawn()` / `spawn_local()` are local-thread APIs. They touch the local fiber
  pool directly and should be called before `run()` starts or from the owner
  scheduler thread.
- `schedule()` is cross-thread safe. It enqueues a move-only function into a
  Vyukov-style MPSC queue and wakes the target `io_context` through `eventfd`.
  The target scheduler drains that queue and creates fibers from its own local
  pool, so fibers never migrate across threads.
- `run()` is run-until-drained. `run_blocking(std::stop_token)` stays alive for
  remote `schedule()` calls until `request_stop()` or the stop token fires.
- The current prototype still does not have full production cancellation
  semantics for unwinding fibers already suspended in I/O.

## Build

The zio benchmark targets are enabled by the existing HTTP benchmark option:

```bash
cmake -S . -B bench-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DKIO_BUILD_HTTP_BENCH=ON \
  -DKIO_BUILD_TESTS=OFF \
  -DKIO_BUILD_DEMOS=OFF

cmake --build bench-build \
  --target zio_http_bench zio_file_io_bench zio_cross_thread_spawn_smoke \
  -j 8
```

The comparable KIO targets are:

```bash
cmake --build bench-build --target kio_fiber_http_bench kio_asio_disk_bench -j 8
```

## HTTP Benchmark

Start zio:

```bash
./bench-build/zio_http_bench --port=8080 --workers=4 --fibers=8192
```

Run oha:

```bash
oha -m GET http://127.0.0.1:8080/ -z 60s -c 1500
```

Latest remote result on `192.168.1.155`:

```text
zio boost::context::fiber:
  Requests/sec: 473,513.20
  Average:      3.1624 ms
  p50:          3.1526 ms
  p90:          4.4186 ms
  p95:          4.5881 ms
  p99:          5.0702 ms

KIO fiber:
  Requests/sec: 437,968.51
  Average:      3.4179 ms
  p99:          5.8266 ms
```

## File I/O Benchmark

Run zio:

```bash
./bench-build/zio_file_io_bench \
  --path=/tmp/kio-zio-disk-bench.dat \
  --bytes=1g \
  --block=64k
```

Comparable KIO command:

```bash
./bench-build/kio_asio_disk_bench \
  --path=/tmp/kio-zio-disk-bench.dat \
  --bytes=1g \
  --block=64k
```

Latest remote result:

```text
zio boost::context::fiber:
  pass 1: 0.248301s  4124.03 MiB/s
  pass 2: 0.231063s  4431.69 MiB/s

KIO fiber:
  0.216578s  4728.09 MiB/s
```

The file benchmark does not fsync and does not drop page cache. Treat it as a
scheduler/io_uring overhead and cached write-throughput benchmark, not durable
storage throughput.

## Cross-Thread Spawn Smoke

The smoke target starts one `zio::io_context` with `run_blocking()`, schedules
jobs into it from producer threads with `schedule()`, and stops the context when
all jobs have run.

```bash
./bench-build/zio_cross_thread_spawn_smoke
```

Expected output:

```text
cross-thread spawn smoke passed jobs=512
```

## Next Optimizations

- Switch Boost.Intrusive hooks from `safe_link` to `normal_link` for Release
  once invariants are stable.
- Add multishot accept.
- Add provided buffers for recv.
- Add fixed file registration.
- Add explicit cancellation/shutdown so pending fibers unwind correctly.
