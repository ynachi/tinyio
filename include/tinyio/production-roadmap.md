# tinyio production roadmap

This roadmap is about turning the current small runtime into a production runtime without losing the clean core design.
The priority is correctness and a clear contract first, then performance.

## Phase 1: contract cleanup

- Remove the fake liburing backend and make the contract explicit: Linux with real liburing only.
- Document the hard invariants:
    - one runtime per owner thread
    - no fiber migration
    - no IO outside a running fiber
    - one in-flight operation per fiber
    - defined `run()` return states
    - defined destruction rules
    - defined file descriptor ownership rules
    - explicit thread-safety boundaries
- Replace ambiguous shutdown behavior with explicit runtime states:
    - `Running`
    - `StopRequested`
    - `Draining`
    - `StoppedClean`
    - `StoppedWithLiveWork`
    - `Fatal`
- Ensure `request_stop()` cannot make `run()` return success while fibers or io_uring operations are still live.

## Phase 2: lifecycle safety

- Add owner-thread stop semantics with clear drain behavior.
- Add a cross-thread wake/post path that can wake a runtime blocked in
  `io_uring_submit_and_wait`.
- Add cancellation for outstanding operations.
- Drain or explicitly fail outstanding completions before destruction.
- Define behavior for blocked `accept`, `read`, `write`, `sleep_for`, and
  `close` during shutdown.
- Add tests for destruction and stop while timeout, read, accept, and write are pending.

## Phase 3: core runtime hardening

- Add checked arithmetic for stack size and guard size alignment.
- Add a bounded stack pool to avoid `mmap`/`mprotect`/`munmap` churn on fiber creation and destruction.
- Drain completion queue entries in batches instead of one `peek_cqe` /
  `cqe_seen` pair at a time.
- Add owner-thread checks for every owner-only API.
- Stabilize the error policy and operation names.
- Add an optional internal log hook for fatal runtime errors.

## Phase 4: real network surface

- Keep the public API small, but add the minimum socket surface needed for real servers:
    - `accept`
    - `read`
    - `write`
    - `close`
    - optionally `recv` and `send` for socket-specific flags
- Add examples for socket setup, keep-alive handling, and graceful shutdown.
- Keep protocol/framework code out of the runtime.

## Phase 5: production testing

- Run the core suite against real liburing.
- Add CI jobs for:
    - Debug
    - Release
    - ASAN
    - TSAN
    - UBSAN
    - Valgrind
    - real io_uring integration tests
    - TCP stress tests
- Add integration tests for:
    - TCP accept/read/write round trip
    - keep-alive request loop
    - client disconnect
    - server-side fd close
    - many concurrent clients
    - partial writes and backpressure
    - shutdown while accept/read/write are pending
    - low SQ/CQ depth
    - low stack size
- Keep longer stress runs outside the fast CI path:
    - 10-30 minute HTTP keep-alive run
    - high connection churn
    - forced client disconnects
    - repeated stop/start lifecycle tests

## Phase 6: performance work

Do this only after the runtime contract is stable.

- Batch CQE draining with liburing's completion queue iteration helpers.
- Tune stack pool size and stack size defaults.
- Add multishot accept.
- Consider multishot recv after the basic socket contract is solid.
- Consider fixed buffers for APIs that can own or register buffers safely.
- Add a multi-runtime `SO_REUSEPORT` example.
- Track scheduler cost per operation, not only end-to-end HTTP RPS.

## Phase 7: API freeze

- Freeze the public API shape.
- Write final invariants, shutdown, and cancellation docs.
- Decide whether `post()` is the only cross-thread API.
- Decide whether cancellation is explicit per operation or runtime-owned.
- Keep examples aligned with the final contract.

## First implementation sprint

1. Remove the fake liburing backend.
2. Fix shutdown semantics.
3. Add cross-thread `post()` with eventfd wakeup.
4. Add a bounded stack pool.
5. Add TCP integration tests.
6. Add batched CQE draining.
7. Re-benchmark against the current baseline.

## How to use tinyio error messages in logs

```c++
        // register pool if not empty                                                                                                                                                                       
        if (buffer_pool_.bucket_count() != 0)                                                                                                                                                               
        {                                                                                                                                                                                                   
            if (auto res = register_buffers(buffer_pool_); !res.has_value())                                                                                                                                
            {                                                                                                                                                                                               
                auto err = res.error();                                                                                                                                                                     
                // We log the context (err.op) and the human-readable system error                                                                                                                          
                ALOG_ERROR("failed to register buffers: [{}] {}", err.op, err.error_code().message());                                                                                                      
            }                                                                                                                                                                                               
            else                                                                                                                                                                                            
            {                                                                                                                                                                                               
                ALOG_INFO("the buffer pool has been registered to io_uring");                                                                                                                               
            }                                                                                                                                                                                               
        }                                                                                                                                                                                                   
                                                                                                                                                                                                            
        ALOG_INFO("Initialized IO context with {} entries (SQPOLL: {})", opts_.entries,                                                                                                                     
                  (opts_.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");        
```