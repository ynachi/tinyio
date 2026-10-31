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
| 5. Timeouts (`Cx::timeout`, timer wheel) | next |
| 6-8 | see the plan |

## Commands

```
make dev        # clippy + gate + tests: what a stage must pass before review
make miri       # nightly: queue / double-push tests under Miri
make bench      # context-switch bench (release)
scripts/ab.sh pingpong   # paired A/B against the C++ build (also: context_switch, sync, post); see the script header
make docker-test
```

Linux only: io_uring (kernel 6.1+ from stage 2), x86_64 and aarch64.
`scripts/gate.sh` is the plan's review gate as a script.

Deviations from the plan, with evidence: [`docs/deviations.md`](docs/deviations.md).
