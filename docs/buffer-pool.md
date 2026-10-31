# The fixed buffer pool: one runtime, one pool

> **Superseded in part.** Written 2026-10-24, when the runtime had exactly one
> pool. The multiclass change (commit `7c835bd`) went back to a list of size
> classes: `IoOptions::buffer_pools` is a vector, and
> `TakeFixedBuffer(slot_size)` / `AcquireFixedBuffer(slot_size)` pick the
> class. Section 1's argument for the single-pool shape no longer applies;
> the mechanics below (chunked registration, FIFO handover, shutdown
> behaviour) still describe each pool correctly.

Status: **LIVING DOCUMENT** — written 2026-10-24, right after the port from
kio landed, while the reasoning was still fresh. Code:
`src/tinyio/runtime/buffer_pool.{hpp,cc}`,
`src/tinyio/runtime/runtime.{hpp,cc}`, tests in
`tests/tinyio/buffer_pool_test.cc` and
`tests/tinyio/shutdown_cancellation_test.cc`.

The facility is kio's, ported: a pool of fixed-size slots carved out of one
huge-page backed mapping and registered with the ring, so a read or a write
can name the slot instead of an address and the kernel does not have to copy
through a bounce buffer. This note records the decisions, not the API — the
API is in the header.

---

## 1. One runtime, one pool

The pool belongs to the runtime:

```cpp
IoOptions opts;
opts.buffer_pool = BufferPoolConfig{.slot_size = 4096, .slots = 256};
IO io(0, opts);
```

The runtime maps it, registers it with the ring at construction, and destroys
it. There is no separate `FixedBufferPool` object for the application to
manage, and no second registration step that can be forgotten or done in the
wrong order.

The earlier shape — a list of size classes, one pool per class, with the
application picking the size — was dropped. It bought the ability to serve
several slot sizes from one runtime, and it cost three things that turned out
to matter more:

- the application had to know the size classes to ask for a buffer at all;
- "which pool does this buffer belong to" became a runtime-wide question,
  and the answer had to be checked on every `ReadFixed`/`WriteFixed`;
- the pools had to be registered together: io_uring takes the whole
  registered-buffer table in one call, and a second
  `io_uring_register_buffers` while a table is registered fails with
  `EBUSY`. A pool that came later meant unregistering and registering
  everything again.

One slot size per runtime keeps the interesting part — the registered buffer
table and the `buf_index` arithmetic — and drops the bookkeeping. A runtime
that needs several slot sizes gets several runtimes.

## 2. No public registration

`io_uring_register_buffers` takes the whole registered-buffer table in one
call. It cannot extend a table that is already registered: a second call
fails with `EBUSY`, and changing the table means unregistering it (or using
the update API on a sparse table). The only simple order is "register
everything at once, before any I/O".

Making registration public turned that constraint into a user-visible rule
("register before any I/O runs, and register all of your pools together"),
which is exactly the kind of rule that gets broken by one person on a good
day. Since the runtime owns its pool, it can register it at construction,
when there is provably nothing else in the table.

The consequence: a buffer that did not come from this runtime's pool cannot
be used with this runtime's `ReadFixed`/`WriteFixed`. That is a programming
error, not an I/O failure, and it terminates with a `CHECK` rather than
quietly naming the wrong buffer through a stale `buf_index`.

## 3. The pool is never closed at shutdown

A parked fiber is a promise that somebody will wake it. The runtime does not
make that promise for the pool: at shutdown, a fiber still parked on a slot
is a leaked lease, and the runtime aborts with the name of the primitive it
is stuck on.

The alternative — closing the pool at shutdown and waking every waiter with
`operation_canceled` — was implemented first and then removed. It looks
gentler, and it is wrong for two reasons:

- **It lies about the cause.** A waiter that is woken with
  `operation_canceled` unwinds as if its operation had been cancelled. But
  nothing was cancelled: the lease it was waiting for was simply never
  released. The application cannot tell "the runtime gave up on me" from
  "my lease leaked", and the leak is the bug worth finding.
- **It is the wrong order.** Waking a waiter lets it destroy a
  synchronization primitive. If an I/O fiber is still unwinding and still
  holds that primitive, the waiter frees it under the I/O fiber's feet.
  Waking pool waiters *after* the I/O fibers unwind fixes the ordering but
  not the first point.

So the pool has a `Close()` for applications that want to retire it early —
it wakes the waiters and is idempotent — but the runtime never calls it on
its own pool.

The pool's destructor does not wake anybody either. `Detach()` drops the
waiters and the owner link before the fibers are destroyed, because a fiber
parked on a slot points at memory that is about to be freed: the pool must
not unpark it afterwards.

## 4. Aborting on parked fibers, and naming them

When the runtime quiesces with a fiber still parked, nothing can ever wake
it. The runtime aborts instead of hanging — a hang in CI is a timeout with
no explanation, an abort is a stack trace.

The message names the culprit. Every parked fiber records *what* it is
waiting on (`WaitKind`: `Mutex`, `Semaphore`, `Latch`, `TaskGroup`,
`FixedBufferPool`), so the abort says:

```
IO #0 quiesced with 2 parked fiber(s) and no one left to wake them:
1 on Latch (fiber 3), 1 on FixedBufferPool (fiber 4)
```

rather than a count plus a list of guesses. The reason lives in the fiber
that is parked, so there is no counter to keep and no bookkeeping to get
wrong; kio needs a counter (`external_parks_`) because a coroutine has
nowhere to store why it suspended, and a stackful fiber does.

## 5. Why the mapping is sliced at 1 GiB

io_uring caps a single registered iovec at 1 GiB, so a bigger mapping has to
be registered as several entries. The pool therefore registers the mapping
as consecutive 1 GiB chunks, and the `buf_index` of a pointer is computed
without any lookup table:

```
buf_index = first_index_ + ((ptr - base_) >> 30)
```

One shift, no interval tree, no per-slot metadata.

This only works because of a constraint on the slot size: **`slot_size` must
be a power of two >= 4096 that divides 1 GiB.** Then every chunk boundary
falls exactly on a slot boundary, and no slot ever straddles two registered
entries. A pool of 4096-byte slots over a 2.5 GiB mapping is three chunks
— two full 1 GiB ones plus a half-GiB tail — and the tail starts exactly
at a slot boundary.

The same reasoning is why the registered table holds a handful of entries
instead of one per slot: a pool of 4096-byte slots over 2.5 GiB is 655 360
slots but **three** iovecs.

The mapping itself is 2 MiB-aligned in both base *and* length, and
`MADV_HUGEPAGE`d, so the kernel can collapse it into 2 MiB compound pages:
fewer page-table walks and TLB misses during DMA.

## 6. Why a released slot is handed to a waiter

When a slot is released while fibers are waiting for one, it is handed
directly to the oldest waiter instead of going back on the free list.

The alternative — push it on the free list, let the next arrival take it —
has a race: the released slot and the newly woken waiter are both "free" at
the same moment, and a third fiber arriving in between can take the slot
that was already promised to the waiter. The waiter then wakes up to find
nothing, and either spins or parks again.

The direct handoff removes the window: the slot is never on the free list
while a waiter exists. Waiters are served strictly FIFO, which also keeps the
assignment deterministic — all slots of a pool are interchangeable, so the
assignment does not have to match, but the *order* does.

The free list itself is LIFO: the most recently used slot is the hot one in
cache.

## 7. What the runtime does at shutdown

1. Cancel every in-flight request with **one** SQE:
   `IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY`. There is no need to
   walk the fibers and cancel them one at a time — at that point every
   request in the ring belongs to the runtime and every one of them has to
   die.
2. Drain the completions. The I/O fibers wake with `ECANCELED` and unwind.
   Sync-parked fibers are **not** woken: they might be waiting for those I/O
   fibers, and waking them early would let them destroy a synchronization
   primitive that is still in use.
3. When the runtime quiesces, if a fiber is still parked, abort — naming the
   primitive, as in §4.

Step 2 is the reason the drain must not give up on the first error. A tick
error used to abandon the drain, leaving operations in flight; the
destructor then exited the ring and freed the fiber stacks, and the kernel
was left referencing `Operation` objects that lived inside those fibers. A
transient `EBUSY` was enough to trigger it.
