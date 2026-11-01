# tinyio-rs

tinyio rebuilt in Rust around its invariants (not around its C++ code), with one
hard rule: a user of the crate never writes `unsafe`.

The spec is [`docs/architecture-plan.md`](docs/architecture-plan.md): the rules (section 2),
the invariants and how Rust enforces each (section 3), the API (section 4), the
unsafe budget (section 5) and the build stages (section 9). The C++ runtime in
`../src/tinyio` is the reference and the benchmark baseline.

## Status

| Stage | State |
|-------|-------|
| 0. Skeleton | done |
| 1. Fibers (context switch, stacks, scheduler, ring-less `Io`) | done |
| 2. I/O core (ring, `syscall`, tick, drain, sleep) | done |
| 3. Sync primitives (`Mutex<T>`, `Semaphore`, `Latch`, `Baton`), `scope` | done |
| 4. `Remote`, `post`, `stop`, `run`, full shutdown | done |
| 5. Timeouts (`Cx::timeout`, timer wheel) | done |
| 6. Net (`net::TcpListener`/`TcpStream`, `accept`/`connect`/`poll`, `std::io` adapter, examples 01-05) | done |
| 7. Fixed buffers (optional) | next |
| 8. Hardening (aarch64 hardware, sanitizers) | see the plan |

## Commands

```
make dev        # clippy + gate + tests: what a stage must pass before review
make miri       # nightly: queue / double-push tests under Miri
make bench      # context-switch bench (release)
cargo run --example 01_hello_echo -- --port=8080   # the five examples are in examples/
scripts/ab.sh pingpong   # paired A/B against the C++ build (also: context_switch, sync, post, disk_read, disk_write); see the script header
make docker-test
```

Linux only: io_uring, x86_64 and aarch64, **kernel 6.9 or later**. 6.9 is the
kernel that added `IORING_OP_FTRUNCATE`, the newest opcode the file API submits;
there is no fallback for an older kernel. `Io::new` asks the kernel which opcodes
it supports (`io_uring_register_probe`, not `uname`, since distributions backport)
and returns an `ErrorKind::Unsupported` error naming the missing one. The C++
runtime keeps its own 6.1 floor.
`scripts/gate.sh` is the plan's review gate as a script.

Deviations from the plan, with evidence: [`docs/deviations.md`](docs/deviations.md).
