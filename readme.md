# tinyio

A low-latency, thread-per-core async I/O runtime for Linux. Built on `io_uring` with stackful fibers.

---

## What it is

tinyio gives each OS thread its own `IO` event loop. Work runs inside **stackful fibers** — ordinary functions that call
blocking-looking I/O operations and suspend cooperatively while the kernel processes them. There are no coroutines, no
`co_await`, no state machines.

```
OS thread
└── IO (event loop)
    ├── fiber: accept loop
    ├── fiber: handle client A
    └── fiber: handle client B
```

Each `IO` instance is **single-threaded** — only the thread that created it may call I/O operations. Scaling is 
done horizontally by spawning multiple IO contexts. Cross-thread dispatch is done through `post()`. For network 
traffic, `SO_REUSE` is a great way to distribute traffic. Tinyio is extremely fast and low latency. It outperformed top  
share nothing libs during most our benchmarks. Also, its tests and demo are always built against sanitizers (ASAN, TSAN, UBSAN) 
and Valgrind to catch memory errors and leaks and fix them.

---

## Requirements

| Item               | Minimum           | Recommended |
|--------------------|-------------------|-------------|
| Linux kernel       | 6.1               | 6.6+        |
| liburing           | 2.8               | 2.12+       |
| Compiler (x86-64)  | Clang 20 / GCC 15 | Clang 22    |
| Compiler (aarch64) | Clang 20 / GCC 16 | Clang 22    |
| C++ standard       | C++23             | —           |

The kernel floor is set by the io_uring features the runtime actually uses,
not by what io_uring could do:

| Feature                                    | Needs |
|--------------------------------------------|-------|
| `IORING_SETUP_SINGLE_ISSUER`               | 6.0   |
| `IORING_SETUP_DEFER_TASKRUN`               | 6.1   |
| `IORING_SETUP_TASKRUN_FLAG`                | 5.19  |
| `IORING_ASYNC_CANCEL_ALL` / `_ANY`         | 5.19  |

`DEFER_TASKRUN` is in the default ring flags, and the shutdown path cancels
every in-flight request with a single `IORING_ASYNC_CANCEL_ALL |
IORING_ASYNC_CANCEL_ANY`. There is no fallback for an older kernel.

---

## Quick start

### Single-threaded, run until done

```cpp
#include "tinyio/tinyio.hpp"
using namespace tinyio;

int main() {
    IO io(0, IoOptions{});

    io.Spawn([](IO& io) noexcept {
        auto fd = TRY(io.Open("/tmp/hello.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644));

        std::string_view msg = "hello from a fiber\n";
        std::span buf(reinterpret_cast<const std::byte*>(msg.data()), msg.size());
        TRY(io.Write(fd, buf));

        TRY(io.Close(std::move(fd)));
    });

    io.RunUntilDone();
}
```

### Multi-threaded HTTP server

Each thread owns one `IO` instance. All threads bind to the same port via `SO_REUSEPORT`.

```cpp
#include "tinyio/tinyio.hpp"
using namespace tinyio;

static Result<void> handle_client(IO& io, Fd client) noexcept {
    std::byte buf[4096];
    while (true) {
        auto n = TRY(io.Read(client, std::span{buf}));
        if (n == 0) break;
        // write response ...
        TRY(io.Write(client, response));
    }
    return {};
}

static Result<void> accept_loop(IO& io, uint16_t port, std::stop_token st) noexcept {
    auto server = TRY(TcpListener::bind(port, "0.0.0.0"));

    while (!st.stop_requested()) {
        auto client = TRY(io.Accept(server));
        io.Spawn([c = std::move(client)](IO& io) mutable noexcept {
            (void)handle_client(io, std::move(c));
        });
    }
    return {};
}

int main() {
    const size_t n_threads = 4;
    std::vector<std::jthread> threads;
    std::vector<std::stop_source> stops;

    for (size_t i = 0; i < n_threads; ++i) {
        stops.emplace_back();
        auto st = stops.back().get_token();
        threads.emplace_back([i, st] {
            IO io(i, IoOptions{});
            io.Spawn([st](IO& io) noexcept {
                (void)accept_loop(io, 8080, st);
            });
            io.Run(st);
        });
    }

    // ... wait for signal, then call stops[i].request_stop() for each
}
```

The full example is in `examples/tinyio/03_http_services.cpp`.

---

## API

### `IO` — the event loop

```cpp
// Construction — takes a worker id, options, and optional fixed buffer config.
IO io(0, IoOptions{});

// Spawn a fiber on the owner thread.
Result<FiberId> io.Spawn([](IO& io) noexcept { ... });

// Run until all fibers finish. No stop token needed.
void io.RunUntilDone();

// Run until the stop token is fired, then drain.
void io.Run(const std::stop_token& st);

// Yield the current fiber back to the scheduler (cooperative multitasking).
Result<void> io.Yield();

// Post a task from any thread. Thread-safe. Returns false if the queue is full.
bool io.Post([](IO& io) noexcept { ... });
```

`IO` is not copyable and not movable. Create it where it will live.

### I/O operations

All operations must be called from inside a running fiber on the owner thread. They suspend the calling fiber until the
kernel completes the operation.

```cpp
// Files
Result<Fd>       io.Open(path, flags, mode = 0644)
Result<void>     io.Close(Fd&&)
Result<uint32_t> io.Read(Fd&, std::span<std::byte>, off_t offset = -1)
Result<uint32_t> io.Write(Fd&, std::span<const std::byte>, off_t offset = -1)
Result<uint32_t> io.Readv(Fd&, std::span<const iovec>, off_t offset = -1)
Result<uint32_t> io.Writev(Fd&, std::span<const iovec>, off_t offset = -1)
Result<void>     io.Fsync(Fd&, bool datasync = false)
Result<void>     io.Fallocate(Fd&, int mode, off_t offset, off_t len)
Result<void>     io.Ftruncate(Fd&, off_t len)   // kernel 6.9+
Result<void>     io.Remove(path)
Result<void>     io.Rename(from, to)

// Network
Result<Fd>       io.Accept(Fd& server, int flags = SOCK_NONBLOCK | SOCK_CLOEXEC)
Result<Fd>       io.Accept(Fd& server, SocketAddress& client_addr, int flags = ...)
Result<void>     io.Connect(Fd&, const SocketAddress&)
Result<uint32_t> io.Poll(Fd&, unsigned poll_mask)

// Fixed (registered) buffers — faster I/O path
Result<uint32_t> io.ReadFixed(Fd&, FixedBuffer&, size_t len, off_t offset = -1)
Result<uint32_t> io.WriteFixed(Fd&, const FixedBuffer&, size_t len, off_t offset = -1)

// Timers
Result<void>     io.SleepForNs(std::chrono::nanoseconds)
```

### `TcpListener` — convenience socket setup

```cpp
// Bind to address + port (IPv4 or IPv6 auto-detected).
Result<Fd> TcpListener::bind(const SocketAddress& addr, int backlog = 4096);
Result<Fd> TcpListener::bind(uint16_t port, const char* ip = nullptr, int backlog = 4096);
```

Sets `SO_REUSEPORT`, `SO_REUSEADDR`, and `TCP_NODELAY` automatically.

### `IoOptions` — tuning

```cpp
IoOptions opts;
opts.sq_entries        = 4096;     // SQ ring size
opts.stack_size        = 256*1024; // fiber stack size
opts.guard_size        = 4096;     // guard page below each stack
opts.max_idle_fibers   = 1024;     // fiber pool high-water mark
opts.batch_max_size    = 128;      // max fibers resumed per tick
opts.tick_timeout_ms   = 10;       // max wait in io_uring_submit_and_wait
opts.worker_cpu_affinity = {0};    // pin to CPU (empty = no pinning)
```

### Error handling

All I/O operations return `Result<T>` which is `std::expected<T, Error>`. `Error` carries a positive errno code and a
static context string.

```cpp
// Manual check
auto res = io.Read(fd, buf);
if (!res) {
    // res.error().code      — errno value
    // res.error().error_code().message() — human-readable string
    // res.error().op        — operation name, e.g. "read"
}

// Rust ?-style propagation (GNU statement expression, Clang/GCC only)
auto n = TRY(io.Read(fd, buf));   // returns error to caller on failure
TRY(io.Fsync(fd));                // Result<void>: just propagate
```

### Fixed buffer pool

Registered buffers bypass the kernel's bounce-buffer copy on every I/O call.

```cpp
// One runtime, one pool: registered with the ring at construction
IoOptions opts;
opts.buffer_pool = BufferPoolConfig{.slot_size = 4096, .slots = 256};
IO io(0, opts);

// Inside a fiber
auto buf = TRY(io.TakeFixedBuffer());   // AcquireFixedBuffer parks the fiber instead
auto n   = TRY(io.ReadFixed(fd, *buf, buf->Size()));
// buf returns to pool automatically on destruction
```

### Synchronization (`TaskGroup`, `Mutex`, `Semaphore`, `Latch`)

tinyio provides fiber-aware synchronization primitives that correctly suspend the fiber without blocking the underlying OS thread.

#### `TaskGroup<T>`
Use `TaskGroup` to run multiple fibers concurrently and wait for them to finish (Structured Concurrency).

```cpp
// 1. Fire-and-forget (TaskGroup<void> deduced)
tinyio::TaskGroup group(io);
group.Spawn([](IO& io) { /* Task 1 */ });
group.Spawn([](IO& io) { /* Task 2 */ });
group.Wait(); // Suspends until all tasks finish

// 2. Collecting results
tinyio::TaskGroup<int> results(io, 2);
results.Spawn([](IO& io) -> Result<int> { return 42; });
results.Spawn([](IO& io) -> Result<int> { return 100; });
results.Wait();
```

### Cross-thread dispatch

```cpp
// From any thread — enqueues a task into the lock-free moodycamel::ConcurrentQueue
// and wakes the owner thread via eventfd.
bool ok = io.Post([](IO& io) noexcept {
    io.Spawn(some_task);
});
// ok == false if the queue is full
```

---

## Building

### From source

```bash
# Configure and build (debug, tests enabled)
cmake --preset dev
cmake --build --preset dev

# Run tests
ctest --preset dev

# Release build
cmake --preset release
cmake --build --preset release
```

Build output lands in `build/`. The presets use `clang-22` / `clang++-22`; edit `CMakePresets.json` to change the
compiler.

### CMake integration

```cmake
find_package(tinyio REQUIRED)
target_link_libraries(my_app PRIVATE tinyio::tinyio)
```

---

## Testing in containers

Tests require a real Linux kernel with io_uring. The `dev-tools/` directory provides a Docker-based environment that
works on both Apple Silicon (arm64) and Intel/Rosetta (amd64) without any Linux VM setup on your part.

### Prerequisites

- Docker Desktop with the containerd image store enabled
- Rosetta enabled in Docker Desktop settings (for amd64 on Apple Silicon)

### Image

Ubuntu 26.04 with clang-22, GCC 15, and liburing 2.14 (the version Ubuntu 26.04 ships natively). All C++ library
dependencies (GTest, Abseil, mimalloc, …) are fetched by CMake FetchContent on first run and cached in an arch-specific
named volume, so subsequent runs are fast.

The container uses the same `dev` CMake preset as local builds. The build volume is mounted at `/workspace/build` (the
preset's `binaryDir`), so the workflow inside the container is identical to working locally:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -L unit
```

### Running tests

```bash
# arm64 — native on Apple Silicon
docker compose -f dev-tools/compose.yaml run --rm test-arm64

# amd64 — via Rosetta on Apple Silicon
docker compose -f dev-tools/compose.yaml run --rm test-amd64
```

By default this runs `ctest --preset dev -L unit` (the fast unit suite). Override with env vars:

```bash
# Run the full test suite (all labels)
TEST_LABEL="" docker compose -f dev-tools/compose.yaml run --rm test-arm64

# Run with AddressSanitizer
TIO_SANITIZER=address docker compose -f dev-tools/compose.yaml run --rm test-arm64

# Run with ThreadSanitizer
TIO_SANITIZER=thread docker compose -f dev-tools/compose.yaml run --rm test-arm64
```

| `TIO_SANITIZER` value | Effect                             |
|-----------------------|------------------------------------|
| `none` (default)      | No instrumentation                 |
| `address`             | ASAN — memory errors, leaks        |
| `thread`              | TSAN — data races on `post()` path |
| `undefined`           | UBSAN — UB in arithmetic and casts |

### Interactive shell

```bash
docker compose -f dev-tools/compose.yaml run --rm test-arm64 bash
# inside the container — identical to local dev:
cd /workspace
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -L unit
```

### Rebuilding the image

The image is built automatically on first `run`. To force a rebuild after changes to `dev-tools/Dockerfile`:

```bash
docker compose -f dev-tools/compose.yaml build
```

---

## Running under sanitizers

You can enable sanitizers during configuration by using CMake options.

### ASAN (AddressSanitizer)

Catches heap/stack overflows, use-after-free, double-free.

```bash
cmake --preset dev -DTINYIO_ENABLE_ASAN=ON -B build_asan
cmake --build build_asan -j$(nproc)

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 ./build_asan/tests/my_test
```

The fiber context switch calls `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber` so ASAN understands
stack ownership. If you see false positives on context-switch frames, verify those hooks are present in
`include/tinyio/detail/sanitizer_hooks.hpp`.

### TSAN (ThreadSanitizer)

Catches data races on the `post()` / MPSC queue cross-thread path.

```bash
cmake --preset dev -DTINYIO_ENABLE_TSAN=ON -B build_tsan
cmake --build build_tsan -j$(nproc)

TSAN_OPTIONS=halt_on_error=1 ./build_tsan/tests/my_test
```

> **Known limitation**: TSAN may segfault at address `0x0000000000d8` on context switches. This is a pre-existing
> interaction between TSAN's fake-stack and the hand-rolled assembly context switch — not a bug in your code. Use
`TSAN_OPTIONS=history_size=7` to get better race reports when it does work.

The fiber switch calls `__tsan_switch_to_fiber` so TSAN knows fiber switches are not races.

### UBSAN (UndefinedBehaviorSanitizer)

Catches signed overflow, null dereference, misaligned access, and shift UB.

```bash
cmake --preset dev -DTINYIO_ENABLE_UBSAN=ON -B build_ubsan
cmake --build build_ubsan -j$(nproc)

./build_ubsan/tests/my_test
```

### Combining ASAN + UBSAN

```bash
cmake --preset dev -DTINYIO_ENABLE_ASAN=ON -DTINYIO_ENABLE_UBSAN=ON -B build_asan_ubsan
```

Do not combine TSAN with ASAN or UBSAN — they are mutually exclusive.

### Valgrind

Slower but catches memory leaks and use of uninitialised values that sanitizers miss, including leaked `mmap` regions
from `allocate_stack`.

```bash
# Build without sanitizers, with debug info
cmake --preset dev -B build_valgrind
cmake --build build_valgrind -j$(nproc)

valgrind \
  --tool=memcheck \
  --leak-check=full \
  --track-origins=yes \
  --error-exitcode=1 \
  ./build_valgrind/tests/my_test
```

For fiber stacks to be tracked correctly, each `mmap`-allocated stack should be registered with
`VALGRIND_STACK_REGISTER` (not yet done — see roadmap Phase 5).

---

## Architecture notes

- **No migration**: a fiber never moves to another `IO` instance. The `IO*` in each fiber is stable for its lifetime.
- **Single issuer**: `IORING_SETUP_SINGLE_ISSUER` is always set. Only the owner thread may submit SQEs. The destructor
  calls `std::abort()` if destroyed from the wrong thread with live work.
- **Deferred task run**: `IORING_SETUP_DEFER_TASKRUN` is on by default. Kernel task callbacks run only inside
  `io_uring_submit_and_wait`, keeping the hot path free of unexpected kernel re-entry.
- **Fiber recycling**: finished fiber stacks are pooled up to `max_idle_fibers` to amortize `mmap`/`mprotect` cost.
- **Cancellation**: when a stop token fires, all waiting fibers receive `ECANCELED` via `IORING_OP_ASYNC_CANCEL`. New
  `syscall()` calls return `ECANCELED` immediately without touching the ring.

---

## License

MIT — see `LICENSE`.
