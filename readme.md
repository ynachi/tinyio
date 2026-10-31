# tinyio

I think high perf software can be simple to understand. I built tinyio for my personal projects after 
trying out various IO libs. I was frustrated to see them as black boxes, where it was pretty hard to really 
understand the lifecycle of an io request.

Tinyio is a low-latency, thread-per-core async I/O runtime for Linux. Built on `io_uring` with stackful fibers.
It is powerful (it outperformed libs like alibaba photonos lib on disk and network IO), yet easy to know exactly what happens 
to you io requests.

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

Each `IO` instance is **single-threaded** — only the thread that created it may call I/O operations. Scaling is done
horizontally by spawning multiple `IO` contexts. Cross-thread dispatch is done through `Post()`; for network traffic,
multiple listeners can share one port via `SO_REUSEPORT`. Every push is built and tested in CI under ASan+UBSan and
TSan, plus a clang-format check.

---

## Requirements

| Item               | Minimum           | Recommended |
|--------------------|-------------------|-------------|
| Linux kernel       | 6.1               | 6.6+        |
| liburing           | 2.9               | 2.9+        |
| Compiler (x86-64)  | Clang 20 / GCC 15 | Clang 22    |
| Compiler (aarch64) | Clang 20 / GCC 16 | Clang 22    |
| C++ standard       | C++23             | —           |

The build pins liburing 2.9 and builds it in the tree; `TINYIO_FETCH_LIBURING=OFF` takes the system one through
pkg-config (still >= 2.9).

The kernel floor is set by the io_uring features the runtime actually uses, not by what io_uring could do:

| Feature                                    | Needs |
|--------------------------------------------|-------|
| `IORING_SETUP_SINGLE_ISSUER`               | 6.0   |
| `IORING_SETUP_DEFER_TASKRUN`               | 6.1   |
| `IORING_SETUP_TASKRUN_FLAG`                | 5.19  |
| `IORING_ASYNC_CANCEL_ALL` / `_ANY`         | 5.19  |

`DEFER_TASKRUN` is in the default ring flags, and the shutdown path cancels every in-flight request with a single
`IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY`. There is no fallback for an older kernel.

---

## Quick start

### Single-threaded, run until done

```cpp
#include "tinyio/tinyio.hpp"
using namespace tinyio;

int main() {
    IO io(0, IoOptions{});

    (void)io.Spawn([](IO& fio) noexcept {
        TRY_LOG_ERR(fd, fio.Open("/tmp/hello.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644),
                    "open failed");

        const std::string_view msg = "hello from a fiber\n";
        const std::span buf(reinterpret_cast<const std::byte*>(msg.data()), msg.size());
        TRY_LOG_ERR_VOID(fio.Write(fd, buf), "write failed");

        TRY_VOID(fio.Close(std::move(fd)));
    });

    io.RunUntilDone();
}
```

### Multi-threaded HTTP server

Each thread owns one `IO` instance. All threads bind to the same port via `SO_REUSEPORT`.

```cpp
#include "tinyio/tinyio.hpp"
using namespace tinyio;

// A static, valid HTTP/1.1 response with keep-alive
constexpr std::string_view kResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, io_uring!";

static Result<void> write_all(IO& fio, const UniqueFd& fd, std::span<const std::byte> data) {
    while (!data.empty()) {
        TRY(written, fio.Write(fd, data));
        if (written == 0) {
            return Error::FailErrno(EIO, "write");
        }
        data = data.subspan(written);
    }
    return {};
}

// Handle a single client connection
static Result<void> handle_client(IO& fio, UniqueFd client_fd) noexcept {
    std::byte buf[1024];

    while (true) {
        TRY(r_n, fio.Read(client_fd, std::span{buf}));
        if (r_n == 0) {
            break;  // peer closed
        }

        const std::span response(reinterpret_cast<const std::byte*>(kResponse.data()),
                                 kResponse.size());
        if (auto res = write_all(fio, client_fd, response); !res) {
            return res;
        }
    }
    return {};
}

// Accept until the runtime stops (Accept then fails with ECANCELED); each
// client gets its own fiber.
static Result<void> accept_loop(IO& fio, uint16_t port) {
    TRY(server_fd, TcpListener::Bind(port, "0.0.0.0"));

    while (true) {
        auto client_res = fio.Accept(server_fd);
        if (!client_res) {
            if (fio.Stopping()) {
                return {};
            }
            return std::unexpected(client_res.error());
        }

        (void)fio.Spawn([client_fd = std::move(*client_res)](IO& cfio) mutable noexcept {
            if (auto res = handle_client(cfio, std::move(client_fd)); !res) {
                ELOGFMT(ERROR, "client handler failed: [{}] {}", res.error().Context(),
                        res.error().Message());
            }
        });
    }
}

int main() {
    constexpr size_t num_threads = 4;
    std::vector<std::jthread> threads;
    std::vector<std::stop_source> stop_sources;

    // Parallel accept on all workers (SO_REUSEPORT lets every thread bind the
    // same port).
    for (std::uint32_t i = 0; i < num_threads; ++i) {
        stop_sources.emplace_back();
        auto st = stop_sources.back().get_token();

        threads.emplace_back([i, st] {
            IO worker(i, IoOptions{});
            (void)worker.Spawn([st](IO& fio) noexcept { (void)accept_loop(fio, 8080); });
            worker.Run(st);
        });
    }

    // ... wait for SIGINT, then stop_sources[i].request_stop() for each
}
```

The full example is in `examples/tinyio/03_http_services.cc`.

---

## API

### `IO` — the event loop

```cpp
// Construction — takes a worker id, options, and optional fixed buffer configs.
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
Result<UniqueFd>  io.Open(path, flags, mode = 0644)
Result<void>      io.Close(UniqueFd)
Result<uint32_t>  io.Read(const UniqueFd&, std::span<std::byte>, off_t offset = -1)
Result<void>      io.ReadExact(const UniqueFd&, std::span<std::byte>, off_t offset = -1)
Result<uint32_t>  io.Write(const UniqueFd&, std::span<const std::byte>, off_t offset = -1)
Result<void>      io.WriteAll(const UniqueFd&, std::span<const std::byte>, off_t offset = -1)
Result<uint32_t>  io.Readv(const UniqueFd&, std::span<const iovec>, off_t offset = -1)
Result<uint32_t>  io.Writev(const UniqueFd&, std::span<const iovec>, off_t offset = -1)
Result<void>      io.Fsync(const UniqueFd&, bool datasync = false)
Result<void>      io.Fallocate(const UniqueFd&, int mode, off_t offset, off_t len)
Result<void>      io.Ftruncate(const UniqueFd&, off_t len)   // kernel 6.9+
Result<void>      io.Remove(path)
Result<void>      io.Rename(from, to)

// Network
Result<UniqueFd>  io.Accept(const UniqueFd& server, int flags = SOCK_NONBLOCK | SOCK_CLOEXEC,
                            std::chrono::nanoseconds timeout = {})
Result<UniqueFd>  io.Accept(const UniqueFd& server, SocketAddress& client_addr,
                            int flags = ..., std::chrono::nanoseconds timeout = {})
Result<void>      io.Connect(const UniqueFd&, const SocketAddress&)
Result<uint32_t>  io.Poll(const UniqueFd&, unsigned poll_mask)

// Fixed (registered) buffers — faster I/O path
Result<uint32_t>  io.ReadFixed(const UniqueFd&, FixedBuffer&, size_t len, off_t offset = -1)
Result<uint32_t>  io.WriteFixed(const UniqueFd&, const FixedBuffer&, size_t len, off_t offset = -1)

// Timers
Result<void>      io.SleepForNs(std::chrono::nanoseconds)
```

### `TcpListener` — convenience socket setup

```cpp
// Bind to address + port (IPv4 or IPv6 auto-detected).
Result<UniqueFd> TcpListener::Bind(const SocketAddress& addr, int backlog = 4096);
Result<UniqueFd> TcpListener::Bind(uint16_t port, const char* ip = nullptr, int backlog = 4096);
```

Sets `SO_REUSEPORT`, `SO_REUSEADDR`, `TCP_NODELAY` (inherited by accepted clients) and `O_NONBLOCK` automatically.

### `IoOptions` — tuning

```cpp
IoOptions opts;
opts.sq_entries         = 4096;     // SQ ring size
opts.stack_size         = 256*1024; // fiber stack size
opts.guard_size         = 4096;     // guard page below each stack
opts.max_idle_fibers    = 1024;     // fiber pool high-water mark
opts.batch_max_size     = 128;      // max fibers resumed per tick
opts.post_queue_capacity = 4096;    // cross-thread Post() queue size
opts.sq_thread_cpu      = -1;       // -1 = no SQ poll thread
opts.sq_thread_idle_ms  = 2000;
opts.worker_cpu_affinity = {0};     // pin to CPU (empty = no pinning)
opts.buffer_pools = {{.slot_size = 4096, .slots = 256}};  // fixed buffer size classes
```

### Error handling

All I/O operations return `Result<T>` which is `std::expected<T, Error>`. `Error` carries a positive errno code and a
static context string.

```cpp
// Manual check
auto res = io.Read(fd, buf);
if (!res) {
    // res.error().Value()   — errno value
    // res.error().Message() — human-readable string
    // res.error().Context() — operation name, e.g. "read"
}

// Rust ?-style propagation: bind the value, propagate the error on failure.
TRY(n, io.Read(fd, buf));           // n is the unwrapped value; returns the error on failure
TRY_VOID(io.Fsync(fd));           // Result<void>: just propagate
```

### Fixed buffer pools

Registered buffers bypass the kernel's bounce-buffer copy on every I/O call. Each entry in
`IoOptions::buffer_pools` is one **size class** with its own pool of slots; a read or write names the slot
and the kernel DMAs directly into it.

```cpp
IoOptions opts;
opts.buffer_pools = {
    {.slot_size = 4096,  .slots = 512},
    {.slot_size = 65536, .slots = 64},
};
IO io(0, opts);

// Inside a fiber
TRY(buf, io.TakeFixedBuffer(4096));        // non-blocking; EAGAIN when that class is empty
TRY(waited, io.AcquireFixedBuffer(4096));  // parks the fiber instead
TRY(n, io.ReadFixed(fd, buf, buf.Size()));
// buf returns to its pool automatically on destruction
```

### Synchronization (`Baton`, `Latch`, `Semaphore`, `Mutex`, `TaskGroup`)

tinyio provides fiber-aware synchronization primitives that correctly suspend the fiber without blocking the
underlying OS thread.

#### `TaskGroup<T>`
Use `TaskGroup` to run multiple fibers concurrently and wait for them to finish (structured concurrency). The
destructor asserts that no tasks are still active, so children cannot outlive the group's scope.

```cpp
// 1. Fire-and-forget (TaskGroup<void> deduced)
tinyio::TaskGroup group(io);
(void)group.Spawn([](IO& io) noexcept { /* Task 1 */ });
(void)group.Spawn([](IO& io) noexcept { /* Task 2 */ });
TRY_VOID(group.Wait());   // suspends until all tasks finish

// 2. Collecting results — capacity is fixed at construction
tinyio::TaskGroup<int> results(io, 2);
(void)results.Spawn([](IO& io) noexcept -> Result<int> { return 42; });
(void)results.Spawn([](IO& io) noexcept -> Result<int> { return 100; });
TRY_VOID(results.Wait());
// results.Results() -> std::vector<std::optional<Result<int>>>
//   nullopt = slot never spawned
//   engaged = check the inner Result<T>: the task's value, its error,
//             or the spawn error pre-filled into the slot
```

### Cross-thread dispatch

```cpp
// From any thread — enqueues a task into a lock-free MPMC queue (the moodycamel
// ConcurrentQueue YLT ships, wrapped in src/tinyio/detail/mpsc_queue.hpp)
// and wakes the owner thread via eventfd.
bool ok = io.Post([](IO& io) noexcept {
    (void)io.Spawn(some_task);
});
// ok == false if the queue is full
```

---

## Building

### From source

The compiler comes from the `CC` / `CXX` environment variables on first configure.

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

Build output lands in `build/<preset>`. There is also a make wrapper for the common tasks: `make dev`, `make asan`,
`make tsan`, `make release` (configure + build + test), `make check` (asan + tsan, what CI runs), `make t T=<pattern>`
(run matching tests), `make fmt` / `make fmt-check`.

### CMake integration

```cmake
find_package(tinyio REQUIRED)
target_link_libraries(my_app PRIVATE tinyio::tinyio)
```

The installed package needs only Threads and a system liburing >= 2.9 (found through pkg-config); YLT's headers
install alongside tinyio's own.

---

## Testing in containers

Tests require a real Linux kernel with io_uring. The `dev-tools/` directory provides a Docker-based environment that
works on both Apple Silicon (arm64) and Intel/Rosetta (amd64) without any Linux VM setup on your part.

### Prerequisites

- Docker Desktop with the containerd image store enabled
- Rosetta enabled in Docker Desktop settings (for amd64 on Apple Silicon)

### Image

Ubuntu 26.04 with clang-22, GCC 15, and liburing 2.14 (the version Ubuntu 26.04 ships natively). The remaining
dependencies (YLT, GTest, mimalloc when enabled) are fetched by CMake FetchContent on first run and cached in an
arch-specific named volume, so subsequent runs are fast.

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
| `address`             | ASan + UBSan                       |
| `thread`              | TSan — data races on `Post()` path |

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

The build has three sanitizer profiles, one CMake preset each:

```bash
cmake --workflow --preset asan   # AddressSanitizer + UndefinedBehaviorSanitizer
cmake --workflow --preset tsan   # ThreadSanitizer
```

Each configures, builds and runs the full test suite. The underlying cache variable is
`TINYIO_SANITIZER` (`none` | `address` | `thread`) if you configure manually. ASan and TSan are mutually exclusive;
UBSan is not separate — the `address` profile carries it.

### ASan (AddressSanitizer + UBSan)

Catches heap/stack overflows, use-after-free, double-free, plus signed overflow, misaligned access and shift UB.

```bash
cmake --preset asan -B build_asan
cmake --build build_asan -j$(nproc)
ctest --preset asan
```

The fiber context switch calls `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber` so ASAN understands
stack ownership. If you see false positives on context-switch frames, verify those hooks are present in
`src/tinyio/detail/sanitizer_hooks.hpp`.

### TSAN (ThreadSanitizer)

Catches data races on the `Post()` / MPMC queue cross-thread path.

```bash
cmake --preset tsan -B build_tsan
cmake --build build_tsan -j$(nproc)
ctest --preset tsan
```

> **Known limitation**: TSAN may segfault at address `0x0000000000d8` on context switches. This is a pre-existing
> interaction between TSAN's fake-stack and the hand-rolled assembly context switch — not a bug in your code. Use
> `TSAN_OPTIONS=history_size=7` to get better race reports when it does work.

The fiber switch calls `__tsan_create_fiber` / `__tsan_switch_to_fiber` so TSAN knows fiber switches are not races.

### Valgrind

Slower but catches memory leaks and use of uninitialised values that sanitizers miss, including leaked `mmap` regions
from stack allocation.

```bash
# Build without sanitizers, with debug info
cmake --preset dev -B build_valgrind
cmake --build build_valgrind -j$(nproc)

valgrind \
  --tool=memcheck \
  --leak-check=full \
  --track-origins=yes \
  --error-exitcode=1 \
  -- ./build_valgrind/tests/<test_binary>
```

For fiber stacks to be tracked correctly, each `mmap`-allocated stack should be registered with
`VALGRIND_STACK_REGISTER` (not yet done).

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

MIT — see `LICENCE`.
