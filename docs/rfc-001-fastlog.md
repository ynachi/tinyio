# RFC-001: FastLog — A FasterLog-Inspired Append-Only Log for tinyio

**Status**: Draft **Author**: tinyio contributors **Created**: 2026-06-18 **Updated**: 2026-06-18 (revised after reading
Microsoft FasterLog source)
**Related**: `docs/faster-sigmod18.pdf`, https://github.com/microsoft/FASTER/tree/main/cs/src/core/FasterLog

---

## Table of Contents

1. [Abstract](#1-abstract)
2. [Background: How FasterLog Works](#2-background-how-fasterlog-works)
    - 2.1 [The FASTER Project](#21-the-faster-project)
    - 2.2 [The Epoch Protection Framework](#22-the-epoch-protection-framework)
    - 2.3 [The Hybrid Log](#23-the-hybrid-log)
    - 2.4 [Append Protocol](#24-append-protocol)
    - 2.5 [Commit Protocol](#25-commit-protocol)
    - 2.6 [Fast Commit Mode](#26-fast-commit-mode)
    - 2.7 [Named / Persisted Iterators](#27-named--persisted-iterators)
    - 2.8 [Read and Scan](#28-read-and-scan)
    - 2.9 [Truncation](#29-truncation)
    - 2.10 [Summary of Complexity Sources](#210-summary-of-complexity-sources)
3. [Motivation](#3-motivation)
4. [Design](#4-design)
    - 4.1 [Scope and Non-Goals](#41-scope-and-non-goals)
    - 4.2 [Address Space](#42-address-space)
    - 4.3 [Record Format and Alignment](#43-record-format-and-alignment)
    - 4.4 [Page](#44-page)
    - 4.5 [Segment Files](#45-segment-files)
    - 4.6 [Append Path](#46-append-path)
    - 4.7 [Page Flusher Fiber](#47-page-flusher-fiber)
    - 4.8 [Commit Waiting and Commit Cookie](#48-commit-waiting-and-commit-cookie)
    - 4.9 [Read Path](#49-read-path)
    - 4.10 [Read Cache](#410-read-cache)
    - 4.11 [Tail Follower and Scan Modes](#411-tail-follower-and-scan-modes)
    - 4.12 [Named / Persisted Iterators](#412-named--persisted-iterators)
    - 4.13 [Truncation](#413-truncation)
    - 4.14 [Recovery](#414-recovery)
5. [Integration with tinyio](#5-integration-with-tinyio)
6. [Configuration Reference](#6-configuration-reference)
7. [Open Questions](#7-open-questions)

---

## 1. Abstract

This RFC specifies FastLog: an append-only, tiered, page-based log designed to run as a first-class component on top of
the tinyio runtime. FastLog is inspired by Microsoft FasterLog but sheds the bulk of FasterLog's complexity by relying
on tinyio's share-nothing, single-threaded-per-shard execution model. The result is a simpler system that provides the
same core guarantees — durable sequential append, logical addressing, forward scan, group commit, efficient tail
following, and named iterator checkpointing — without epoch protection, lock-free data structures, or cross-thread
memory hazards.

FastLog's primary intended use cases are:

- Per-shard write-ahead log (WAL) for a storage engine
- Durable event/commit log for pub-sub consumers
- Replication log shipped to replica nodes

---

## 2. Background: How FasterLog Works

This chapter is a self-contained introduction to FasterLog for readers without prior exposure to the FASTER project.
Familiarity with the original paper (`faster-sigmod18.pdf`) is not assumed. The descriptions in this chapter are
grounded in the actual C# source at `microsoft/FASTER/cs/src/core/FasterLog`.

### 2.1 The FASTER Project

FASTER is a research system from Microsoft Research (SIGMOD 2018, Chandramouli et al.). It addresses a specific problem:
how do you build a key-value store that runs at hundreds of millions of operations per second on a single machine,
across many CPU cores, without becoming bottlenecked on synchronization?

The two headline innovations in FASTER are:

1. **The epoch protection framework** — a replacement for locks and RCU that lets threads operate on shared data
   structures without ever acquiring a lock on the common path.
2. **The hybrid log** — a tiered storage structure that keeps hot data in DRAM and cold data on disk, with a design that
   avoids random I/O even for updates.

FasterLog is the log component of FASTER. It was designed as a general-purpose append-only log that reuses the epoch
framework and the same tiered storage model. It is used internally as FASTER KV's on-disk log and is also exposed as a
standalone library (`FasterLog.cs`).

### 2.2 The Epoch Protection Framework

This is the core piece of intellectual machinery behind all of FASTER. Understanding it is essential to understanding
why FasterLog is designed the way it is — and why we can drop it entirely.

**The problem it solves**: multiple threads operating on shared mutable state. The classic solution is a mutex. Mutexes
are slow because they serialize access and cause context switches. RCU (Read-Copy-Update) is faster for read-heavy
workloads but complex. FASTER introduces epochs as a simpler, faster alternative suited to the specific access patterns
of a log and KV store.

**How it works**:

Every thread that wants to access shared state calls `Protect()` before doing so and `Unprotect()` afterward. These are
cheap operations (essentially a store and a load of an integer).

The system maintains a global epoch counter `E`. Each call to `Protect()` records the current epoch in a thread-local
slot. An epoch "drains" when all threads have called `Protect()` with a value ≥ that epoch (i.e., no thread is still
inside an old epoch).

Actions that would be unsafe to perform while some thread is still in an old epoch are **deferred**: they are placed in
a trigger table and executed only once the epoch they depend on has drained. A thread that calls `BumpEpoch()`
increments the global counter and optionally registers a deferred action.

Example of how this replaces a lock in FasterLog's page flusher:

- Before starting a page flush, the flusher bumps the epoch and registers: "when this epoch drains, advance
  `CommittedUntilAddress`."
- Threads that were mid-append in the old epoch will finish and call `Unprotect()`.
- Once all threads have left the old epoch, the deferred action fires and `CommittedUntilAddress` advances.
- No lock was ever held during any append or during the epoch drain check.

The cost of `Protect()` / `Unprotect()` is roughly one atomic store each — much cheaper than a mutex acquire/release,
and it never blocks.

**The complexity cost**: the epoch framework is not trivial to implement correctly. It requires careful cache-line
alignment of per-thread epoch slots (to avoid false sharing), correct ordering of deferred triggers, and correct
handling of threads that join or leave the thread pool. It is also deeply entangled with every data structure that
FASTER exposes — you cannot use a FASTER data structure from a thread that has not called `Protect()`.

### 2.3 The Hybrid Log

FasterLog's storage is divided into regions along the logical address space, controlled by three monotonically advancing
cursors:

```
  BeginAddress    CommittedUntilAddress   SafeTailAddress   TailAddress
       │                  │                    │                │
       ▼                  ▼                    ▼                ▼
───────┼──────────────────┼────────────────────┼────────────────┼──▶
       │   immutable      │   fuzzy / flushing │  in memory     │
       │   (on disk)      │   (in transition)  │  (mutable)     │
```

- **`TailAddress`**: next byte to reserve. Advanced by `fetch_add` on every enqueue. Records may not be fully written
  yet after advancing.
- **`SafeTailAddress`**: highest address up to which all records are fully written to memory (no in-flight writes from
  other threads). Always ≤ `TailAddress`. Maintained by `AutoRefreshSafeTailAddress` or explicit calls. Consumers
  scanning uncommitted data can safely read up to this address.
- **`CommittedUntilAddress`**: highest address known to be durable on disk. Advances in page-sized steps after
  successful flushes.
- **`BeginAddress`**: lowest valid address. Advances on truncation.

All four are 64-bit logical addresses that only move forward. The in-memory region is backed by a circular buffer of
page-sized slots, indexed by `(page_number % BUFFER_CAPACITY)`.

### 2.4 Append Protocol

Multiple threads can append to FasterLog concurrently. The actual C# API is `Enqueue` / `TryEnqueue`.

1. Thread calls `Protect()` to enter an epoch.
2. Thread does an atomic `fetch_add` on `TailAddress` by `headerSize + Align(payloadLength)`. This is the only
   synchronization point on the append fast path.
3. The returned old value is the logical address. The thread writes its record header then payload into the page buffer
   at that address.
4. Thread calls `Unprotect()`.

**Record alignment**: every record is padded to a 4-byte boundary. `Align(n) = (n + 3) & ~3`. The scanner uses the same
formula to advance its cursor, skipping any padding bytes.

**Header format**: FasterLog uses two header sizes depending on `LogChecksumType`:

- `LogChecksumType.None` → 4-byte header (payload length only)
- `LogChecksumType.PerEntry` → 12-byte header (4 bytes length + 8 bytes checksum)

Because multiple threads can be mid-write simultaneously, a sealed page may still have in-flight writes from threads
that reserved space before the seal but have not finished writing. The epoch framework resolves this: the flusher bumps
the epoch before reading a page for flushing, so by the time the old epoch drains all in-flight writes to that page are
complete.

### 2.5 Commit Protocol

FasterLog uses **group commit**: many appends are made durable in one I/O operation (a page write).

Commit is an explicit operation (`Commit()` / `CommitAsync()`). It is controlled by a pluggable `LogCommitPolicy`:

- **Default**: admits a commit if the tail has advanced since the last committed address or if metadata (iterators,
  cookie) changed. Drops redundant commits.
- **MaxParallel (k)**: at most `k` commits in flight simultaneously; extra calls are auto-retried.
- **RateLimit (thresholdMs, thresholdBytes)**: admits a commit only if at least `thresholdBytes` bytes have accumulated
  or `thresholdMs` milliseconds have elapsed since the last commit. Useful for capping I/O rate.

The commit protocol itself:

1. Snapshot `TailAddress` as the commit target (`CommitTail`).
2. Serialize a `FasterLogRecoveryInfo` containing: `BeginAddress`, `UntilAddress` (= `CommitTail`), `CommitNum`
   (monotone counter), persisted iterator positions, and an optional user cookie.
3. Flush all pages up to `CommitTail` to disk via the I/O device.
4. Once flush completes, call `ILogCommitManager.Commit(beginAddress, untilAddress, metadata, commitNum)` which persists
   the `FasterLogRecoveryInfo` to a separate commit metadata file.
5. Advance `CommittedUntilAddress` to `CommitTail`.
6. Wake all `WaitForCommit` callers.

Callers can `WaitForCommit(untilAddress)` or `WaitForCommitAsync(untilAddress)` to block until
`CommittedUntilAddress ≥ untilAddress`.

The `FasterLogRecoveryInfo` serialized to disk contains a checksum
(`BeginAddress ^ UntilAddress ^ CommitNum ^ iteratorCount ^ cookieLength ^ cookieChecksum`) to detect corruption.

### 2.6 Fast Commit Mode

In standard mode, commit metadata is written to a separate file managed by `ILogCommitManager` (by default, a
`DeviceLogCommitCheckpointManager`). This requires two I/Os per commit: the page write plus the metadata write.

**Fast commit mode** (`FastCommitMode = true`) eliminates the separate metadata write by embedding commit records
directly inline in the log stream. A special "commit record" is written as an ordinary record with a flag in the header;
its payload is the serialized `FasterLogRecoveryInfo`.

Tradeoffs:

- **Faster commits**: one fewer I/O, lower latency.
- **Slower recovery**: on restart, FasterLog must scan forward through the log to find the last commit record, rather
  than reading a known metadata file. Recovery time is O (data written since last truncation) instead of O (1).
- **Requires checksums**: `LogChecksumType.PerEntry` must be enabled so the scanner can detect real records vs commit
  records vs padding.

Scanners must recognize and skip commit records (`isCommitRecord` flag in the internal `GetNextInternal` method).

### 2.7 Named / Persisted Iterators

FasterLog supports **named iterators**: scan iterators registered with a string name whose read position is checkpointed
as part of every `Commit()`.

```csharp
// Create a named iterator — position is persisted with each commit
using var iter = log.Scan(beginAddress, long.MaxValue, name: "consumer-1");
```

The `FasterLog` instance maintains a `ConcurrentDictionary<string, FasterLogScanIterator> PersistedIterators`. On every
commit, `FasterLogRecoveryInfo.SnapshotIterators()` records the current `CompletedUntilAddress` of each named iterator
in the `Iterators` dictionary of the commit record.

On recovery, `RecoveredIterators` is populated from the last commit record. When a named iterator is re-created via
`Scan(name: "consumer-1", recover: true)`, it resumes from its checkpointed position rather than from `beginAddress`.

This enables **exactly-once consumer semantics**: if the consumer crashes between reads, on restart it picks up from the
last position it explicitly advanced via `CompleteUntil(address)`.

The consumer is responsible for calling `CompleteUntil(address)` after successfully processing each record. This is
deliberately separate from just advancing the scan cursor, allowing the consumer to do its own processing (e.g., writing
to a downstream system) before declaring a record consumed.

### 2.8 Read and Scan

Reading a record at a known logical address:

- If `address ≥ CommittedUntilAddress` (and `address < SafeTailAddress`): record is in the uncommitted in-memory region.
  Accessible only if `scanUncommitted = true`.
- If `address ≥ CommittedUntilAddress` (and `scanUncommitted = false`): not yet committed; block or return not-ready.
- If `address ≥ BeginAddress`: record is on disk (immutable). Read via buffered page fetch.
- If `address < BeginAddress`: truncated; return error.

Forward scan advances `cursor += headerSize + Align(entryLength)` after each record. No index needed.

**Scan buffering modes** (`ScanBufferingMode`):

- `SinglePageBuffering`: one page-sized buffer. When the scanner crosses a page boundary and the new page is on disk, it
  issues a synchronous async read and waits.
- `DoublePageBuffering` (default): two page-sized buffers. While processing the current page, the next page is being
  loaded from disk in the background (prefetch). For sequential scans over cold data, this roughly halves wait time.

The iterator (`FasterLogScanIterator`) exposes:

- `GetNext()` — synchronous, returns false if no more data available right now
- `GetAsyncEnumerable()` — async C# enumerable that awaits new data when the cursor reaches `CommittedUntilAddress`
- `WaitAsync()` — await until new data or commit arrives

### 2.9 Truncation

`TruncateUntilAddress(address)` advances `BeginAddress` to `address`. This frees in-memory page slots below `address`
and allows segment files fully below `address` to be deleted. Truncation does not block appenders or readers above
`address`.

### 2.10 Summary of Complexity Sources

| Complexity                                    | Why it exists in FasterLog                                                                     |
|-----------------------------------------------|------------------------------------------------------------------------------------------------|
| Epoch protection framework                    | Multiple threads append concurrently; need memory-safe access without locks                    |
| `SafeTailAddress` distinct from `TailAddress` | In-flight concurrent appends to same page create a gap between reserved and fully-written tail |
| Lock-free `fetch_add` on `TailAddress`        | Concurrent reservation of space in the page buffer                                             |
| Deferred trigger table                        | Epoch-based deferral of `CommittedUntilAddress` advancement                                    |
| Thread registration/management                | Every thread must register with the epoch system before use                                    |
| Callback/`Task`-based commit wait             | No coroutine/fiber support in the C# implementation                                            |
| `ILogCommitManager` abstraction               | Support pluggable storage backends for commit metadata                                         |
| Fast commit mode scanner complexity           | Inline commit records must be recognized and skipped during scan                               |

All of these except named iterators, commit cookie, and scan buffering mode exist purely to handle concurrent
multi-threaded access to shared log state. In tinyio, the log is owned by a single IO thread. The concurrency complexity
vanishes entirely.

---

## 3. Motivation

The FASTER project is a compelling piece of systems engineering, but its complexity is driven by a specific requirement:
maximally concurrent access from many threads. tinyio's execution model gives us a different set of constraints:

- Each `IO` instance runs on exactly one OS thread.
- All operations on a log instance are issued by fibers running on that thread.
- Cross-thread access is routed through `post()`, serializing it on the owning thread before it reaches the log.

This means within a single log instance:

- There is exactly one appender active at a time (the current fiber on the IO thread).
- `TailAddress == SafeTailAddress` always — no in-flight writes from other threads, ever.
- There is no concurrent mutation of any data structure.
- No epoch protection is needed.
- No lock-free atomics are needed on the append fast path.
- There is no fuzzy region: when a fiber seals a page, no other writer is mid-write to it.

We retain all of FasterLog's useful features — tiered storage, logical addresses, page-based I/O, group commit, forward
scan with double-page prefetch, scan-uncommitted, named iterator checkpointing, commit cookie — with a much simpler
implementation that maps naturally onto io_uring async I/O and fiber-based blocking.

---

## 4. Design

### 4.1 Scope and Non-Goals

**In scope**:

- Append-only sequential write
- Logical addressing (monotonic u64 offset)
- Page-based group commit to disk via io_uring
- Forward scan by logical address with double-page prefetch buffering
- Async tail following: blocking fiber until new committed or uncommitted data arrives
- Named / persisted iterators with `complete_until()` checkpointing
- Commit cookie: arbitrary user bytes attached to each commit
- Truncation with segment file deletion
- Crash recovery from commit metadata
- SIEVE-based read cache for on-disk pages

**Not in scope** (for this RFC):

- Cross-log transactions (handled at a higher layer)
- Replication (log pages can be shipped by a separate fiber; not part of this design)
- Fast commit mode / inline commit records (future extension; see §7)
- Compression
- Encryption
- Multiple concurrent writers to the same log from multiple IO threads (use `post()` to serialize on the owner)

### 4.2 Address Space

The log exposes a 64-bit logical address space that grows monotonically. Three cursors partition it:

```
  head_offset       flush_offset     tail_offset
       │                  │               │
       ▼                  ▼               ▼
───────┼──────────────────┼───────────────┼──────▶  logical address (u64)
       │   on disk        │  in memory    │
       │   (immutable)    │  (mutable)    │
```

- `tail_offset`: next byte to write. Advances on every append. Because the log is single-threaded, this is also the safe
  tail — there are never in-flight partial writes.
- `flush_offset`: highest offset known to be durable on disk. Advances in `PAGE_SIZE` increments when a page flush
  completes. Always page-aligned.
- `head_offset`: lowest valid offset. Advances on truncation. Always page-aligned.

All three are `uint64_t`. On a 64-bit machine with 256 KB pages, the address space holds 4.7 × 10¹² pages — effectively
unlimited.

**Page number and offset within page** are derived from a logical address:

```
page_number      = address / PAGE_SIZE
offset_in_page   = address % PAGE_SIZE
```

### 4.3 Record Format and Alignment

Records are packed sequentially into pages. Each record is followed by alignment padding to the next 4-byte boundary
(matching FasterLog's `Align()` convention). Scanners must apply the same formula to advance the cursor.

```
 0       1       2       3       4       5       6       7
 ┌───────┬───────┬───────┬───────┬───────────────────────┐
 │ magic │ magic │  payload_len (uint32_t, LE)            │
 ├───────┴───────┴───────┴───────┴───────────────────────┤
 │                  payload (variable)                    │
 ├───────────────────────────────────────────────────────┤
 │  crc32c (uint32_t, LE)  │  padding to 4-byte boundary │
 └─────────────────────────┴───────────────────────────┘
```

- `magic`: 2 bytes, fixed value `0xFA 0x57`. Used to detect corruption and to distinguish real records from zero-padded
  page tails during recovery scan. FasterLog does not use magic bytes; we add them for robustness.
- `payload_len`: 4 bytes, little-endian unsigned. Length of the payload in bytes. Does **not** include the header,
  checksum, or padding.
- `payload`: variable-length application data.
- `crc32c`: 4 bytes, little-endian. CRC32C over `magic + payload_len + payload`. Verified on read. Hardware-accelerated
  on x86 (SSE4.2) and aarch64 (ARMv8 CRC extension).
- `padding`: 0–3 zero bytes so that the next record starts at a 4-byte aligned offset.

**Total per-record overhead**: 10 bytes fixed + 0–3 bytes padding.

**Alignment formula** (same as FasterLog):

```cpp
static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }

// Slot consumed in the page by one record:
uint32_t slot_size(uint32_t payload_len) {
    return 10u + align4(payload_len) - payload_len  // wait, let me recalculate
}
// Simpler: total bytes written = 6 (header) + payload_len + 4 (crc) = 10 + payload_len
// Bytes to advance cursor = align4(10 + payload_len)
uint32_t record_stride(uint32_t payload_len) {
    return align4(6u + payload_len) + 4u;  // header + aligned payload + crc
}
```

Wait — to keep alignment simple and consistent with FasterLog: treat `(header + payload)` as the alignable unit, then
append the CRC. Alternatively, align the entire record including CRC:

```cpp
// Stride = align4(magic(2) + len(4) + payload(N) + crc(4))
//        = align4(10 + N)
uint32_t record_stride(uint32_t payload_len) {
    return align4(10u + payload_len);
}
```

This is the convention we adopt. The cursor always advances by `align4(10 + payload_len)` after each record.

**Max single-record payload**: `PAGE_SIZE - 10` bytes. Larger payloads require fragmentation (future extension; see §7).

### 4.4 Page

A page is a fixed-size raw byte buffer. It is the unit of I/O between memory and disk.

```cpp
static constexpr uint32_t PAGE_SIZE = 256 * 1024;  // 256 KB, must be power of two

struct alignas(4096) Page {
    uint8_t  data[PAGE_SIZE];
    uint32_t written;   // bytes used; records are packed in data[0..written)
    enum class State : uint8_t {
        Mutable,   // current write target
        Sealed,    // full or explicitly sealed; awaiting flush
        Flushing,  // io_uring write in flight
        Flushed,   // durable on disk; slot reusable
    } state;
};
```

Pages are stored in a circular ring buffer in memory, sized `BUFFER_PAGES` entries. The ring slot for page `N` is
`N % BUFFER_PAGES`. `BUFFER_PAGES` must be large enough that the flusher can finish flushing old pages before the
appender wraps around and needs their slots. A safe default is 32 (8 MB in-memory buffer with 256 KB pages).

**Page boundary handling**: a record is never split across pages. If the current page does not have enough remaining
space for the next record (`align4(10 + payload_len)` bytes), the remainder of the page is zero-padded and the page is
sealed. The record is written at the start of the next page. Scanners detect the zero-padded tail by checking for a zero
or invalid magic and skipping to the next page boundary.

**Large records** (payload exceeding `PAGE_SIZE - 10`) are fragmented: each fragment carries a continuation flag in the
header. Fragmentation is rare in the WAL use case; for now we cap single-record payloads at `PAGE_SIZE - 10` and
document fragmentation as a future extension (§7).

### 4.5 Segment Files

The on-disk address space is divided into segment files, each covering a fixed range of the logical address space.

```
Segment 0:  log_00000000.seg   offsets [0,           SEGMENT_SIZE)
Segment 1:  log_00000001.seg   offsets [SEGMENT_SIZE, 2*SEGMENT_SIZE)
...
```

`SEGMENT_SIZE` must be a multiple of `PAGE_SIZE`. Default: 512 MB.

The segment file for address `A` is `A / SEGMENT_SIZE`. The byte offset within that file is `A % SEGMENT_SIZE`.

Each segment file is pre-allocated with `fallocate` (via io_uring) before it is needed, so page writes never extend a
file — they always overwrite pre-allocated space. The segment allocator fiber opens and preallocates the next segment
file when the flusher reaches 80% of the current one.

Segment files are opened with `O_DIRECT | O_RDWR`. All reads and writes are page-aligned, satisfying `O_DIRECT`
alignment requirements. The kernel page cache is bypassed entirely; FastLog is its own page cache.

**Commit metadata** is stored separately from log data, in a `log_commit/` subdirectory. Each commit produces one file
named by `commit_num` (zero-padded hex). This mirrors FasterLog's `DeviceLogCommitCheckpointManager` approach and keeps
the log files themselves pure data.

### 4.6 Append Path

The append path is synchronous from the caller's perspective — it writes bytes into the in-memory page buffer and
returns a logical address immediately. No I/O is issued on the append path.

```
LogAddress FastLog::append(std::span<const uint8_t> payload):

  1. stride   = align4(10 + payload.size())
  2. If stride > PAGE_SIZE - current_page.written:
       seal current page (zero-pad remainder, set state = Sealed)
       notify flusher fiber
       advance to next page slot (wrapping in ring)
       if next slot is not Flushed: return Err::buffer_full  // back-pressure
       initialize next page (written = 0, state = Mutable)
  3. addr = tail_offset
  4. Write into current_page.data[current_page.written]:
       write magic (2 bytes: 0xFA 0x57)
       write payload_len (4 bytes LE)
       write payload (payload.size() bytes)
       write crc32c (4 bytes) over [magic + payload_len + payload]
       write zero padding ((stride - 10 - payload.size()) bytes)
  5. current_page.written += stride
  6. tail_offset           += stride
  7. notify tail_waiters   (wake any fiber waiting on scan-uncommitted)
  8. return addr
```

No atomics. No locks. No kernel calls. The return value is the logical address of the record, usable for direct reads or
for `wait_until_flushed()`.

If `buffer_full` is returned, the caller should yield to the event loop once to allow the flusher fiber to make
progress, then retry. This is the natural back-pressure mechanism.

### 4.7 Page Flusher Fiber

A background fiber that runs on the same IO thread as the log. It wakes when a page is sealed and flushes sealed pages
in order.

```
void FastLog::flusher_fiber():

  loop:
    wait for: at least one page in state Sealed
              or shutdown signal

    for each sealed page P in ascending order of page_number:
      P.state = Flushing
      seg_fd  = segment_file_for(P.page_number)
      offset  = (P.page_number * PAGE_SIZE) % SEGMENT_SIZE
      co_await io_uring_write(seg_fd, P.data, PAGE_SIZE, offset)
      P.state = Flushed
      flush_offset = (P.page_number + 1) * PAGE_SIZE
      wake all commit_waiters with target_offset <= flush_offset

    if flush_offset approaching next segment boundary:
      ensure next segment file exists (preallocate if needed)
```

Key properties:

- Pages are always flushed **in ascending order**. `flush_offset` never has gaps.
- Multiple sealed pages can be submitted to io_uring as a batch before awaiting any — the disk subsystem can
  reorder/merge internally. However, `flush_offset` advances only strictly in page order: page N+1 completing does not
  advance `flush_offset` past page N.
- `O_DIRECT` writes require the buffer to be 4096-byte aligned. `alignas(4096)` on `Page` satisfies this.
- **Relaxed durability** (default): advance `flush_offset` on `io_uring` write completion (data in disk controller
  buffer).
- **Strict durability**: issue `io_uring_prep_fsync(IORING_FSYNC_DATASYNC)` after each page (or after each batch) and
  advance `flush_offset` only on `fdatasync` completion.

### 4.8 Commit Waiting and Commit Cookie

**Commit waiting**: a fiber that needs durability at a given address suspends until `flush_offset` reaches that address.

```cpp
struct CommitWaiter {
    uint64_t    target_offset;
    FiberHandle fiber;
    byte[]      cookie;    // copied in, returned to waiter on wake
};

std::vector<CommitWaiter> commit_waiters_;  // kept sorted by target_offset

Awaitable FastLog::wait_until_flushed(LogAddress addr,
                                       std::span<const uint8_t> cookie = {}) {
    if (flush_offset_ >= addr) return immediately;
    commit_waiters_.push_back({addr, current_fiber(), {cookie.begin(), cookie.end()}});
    co_yield;
    // resumed by flusher when flush_offset >= addr
}
```

When the flusher advances `flush_offset`, it wakes all waiters whose `target_offset ≤ new flush_offset` in one pass.
This is implicit group commit: all appends that accumulate during one page's flush cycle are committed together when
that page is flushed.

**Commit cookie**: borrowed directly from FasterLog's `FasterLogRecoveryInfo.Cookie`. An arbitrary byte blob the caller
attaches to a commit. It is persisted in the commit metadata file alongside `flush_offset` and named iterator positions.
On recovery, the cookie from the last commit is returned to the application.

Use cases:

- A WAL writer attaches the B-tree checkpoint offset as a cookie, so on recovery it knows exactly which WAL entries
  still need replay.
- A pub-sub producer attaches a producer sequence number, enabling idempotent re-publish after crash.

```cpp
// Writing a WAL entry and attaching a checkpoint cookie:
LogAddress addr = log.append(wal_record);
CheckpointMeta meta = {.btree_lsn = current_lsn};
co_await log.wait_until_flushed(addr, as_bytes(meta));
// meta is now persisted alongside the commit record
```

The commit metadata file stores: `flush_offset`, `head_offset`, `commit_num`, named iterator positions, cookie length +
cookie bytes, and a checksum of all fields (matching FasterLog's `ToByteArray()` format for potential
cross-compatibility).

### 4.9 Read Path

```
Record FastLog::read(LogAddress addr):

  if addr < head_offset_:    return Err::truncated
  if addr >= tail_offset_:   return Err::out_of_range

  if addr >= flush_offset_:
    // in memory — direct pointer into page buffer, no I/O
    page = ring_[page_number(addr) % BUFFER_PAGES]
    return decode_record(page.data + offset_in_page(addr))

  // on disk — check read cache
  page = read_cache_.get(page_number(addr))
  if not page:
    page = co_await fetch_page_from_disk(page_number(addr))
    read_cache_.insert(page_number(addr), page)
  return decode_record(page.data + offset_in_page(addr))
```

`decode_record` verifies magic bytes and CRC32C; returns `Err::corrupt` on mismatch.

`fetch_page_from_disk`:

```
  seg_fd = segment_file_for(page_number)
  buf    = allocate aligned buffer (PAGE_SIZE, alignment 4096)
  co_await io_uring_read(seg_fd, buf, PAGE_SIZE, byte_offset_in_segment)
  return buf
```

### 4.10 Read Cache

The read cache holds recently fetched on-disk pages. It uses the SIEVE eviction algorithm (Zhang et al., 2023) in
preference to LRU.

**Why SIEVE over LRU**: the dominant read patterns for a log read cache are sequential scans (recovery replay, consumer
catch-up). LRU is polluted by sequential scans — a one-time pass over 1 GB of pages evicts everything that was hot
before. SIEVE is scan-resistant: sequentially accessed pages that are not revisited get their visited bit reset by the
hand and are evicted without disturbing frequently re-accessed pages.

```cpp
struct SieveCache {
    struct Slot {
        uint64_t page_number = UINT64_MAX;  // UINT64_MAX = empty
        Page*    page        = nullptr;
        bool     visited     = false;
    };

    std::vector<Slot>                       ring_;
    size_t                                  hand_ = 0;
    std::unordered_map<uint64_t, size_t>    index_;  // page_number -> slot index

    Page* get(uint64_t pn) {
        auto it = index_.find(pn);
        if (it == index_.end()) return nullptr;
        ring_[it->second].visited = true;   // O(1), no reorder
        return ring_[it->second].page;
    }

    void insert(uint64_t pn, Page* page) {
        size_t slot = evict_one_();
        ring_[slot] = {pn, page, false};
        index_[pn]  = slot;
    }

    void evict_below(uint64_t below_page_number) {
        for (auto& [pn, slot] : index_)
            if (pn < below_page_number) ring_[slot] = {};
        std::erase_if(index_, [&](auto& kv){ return kv.first < below_page_number; });
    }

private:
    size_t evict_one_() {
        while (ring_[hand_].visited) {
            ring_[hand_].visited = false;
            hand_ = (hand_ + 1) % ring_.size();
        }
        if (ring_[hand_].page_number != UINT64_MAX)
            index_.erase(ring_[hand_].page_number);
        size_t s = hand_;
        hand_ = (hand_ + 1) % ring_.size();
        return s;
    }
};
```

Default cache size: 64 slots (16 MB with 256 KB pages). Configurable. For WAL use (write-heavy, rare reads), 8 slots is
sufficient.

### 4.11 Tail Follower and Scan Modes

A scanner reads records sequentially from a starting address. Two scan modes exist, mirroring FasterLog's
`scanUncommitted` flag:

**Committed scan** (default): the cursor only advances up to `flush_offset`. When `cursor >= flush_offset`, the fiber
suspends until the flusher advances `flush_offset`.

**Uncommitted scan**: the cursor can advance up to `tail_offset` (which in our single-threaded model is always fully
written, so it equals FasterLog's `SafeTailAddress`). Records not yet durable on disk are visible. Useful for replica
log shippers that can tolerate re-reading on crash.

**Double-page prefetch** (mirroring FasterLog's `ScanBufferingMode.DoublePageBuffering`): when reading from disk, two
page-sized read buffers are maintained. While the scanner processes records from page N, page N+1 is being loaded
asynchronously. The prefetch is kicked off as soon as the cursor enters the second half of page N.

```cpp
struct Scanner {
    FastLog*   log;
    LogAddress cursor;
    bool       scan_uncommitted = false;

    // Two prefetch slots (double buffering for disk pages)
    struct PrefetchSlot {
        uint64_t   page_number = UINT64_MAX;
        Page*      page        = nullptr;
        bool       in_flight   = false;
    } prefetch_[2];

    Awaitable<Record> next() {
        uint64_t limit = scan_uncommitted ? log->tail_offset_
                                          : log->flush_offset_;
        if (cursor >= limit) {
            // register as waiter on the appropriate counter
            if (scan_uncommitted)
                log->tail_waiters_.push_back({cursor, current_fiber()});
            else
                log->commit_waiters_.push_back({cursor + 1, current_fiber()});
            co_yield;
            limit = scan_uncommitted ? log->tail_offset_ : log->flush_offset_;
        }

        // Ensure current page is available (hit in-memory ring or read cache)
        Page* page = co_await get_page_(page_number(cursor));

        // Parse record
        uint8_t* ptr = page->data + offset_in_page(cursor);
        // check magic, decode payload_len, verify CRC32C ...
        Record r = decode(ptr);

        uint32_t stride = align4(10u + r.payload_len);
        cursor += stride;

        // Skip zero-padded tail if we've crossed a page boundary
        if (offset_in_page(cursor) < 10 && cursor % PAGE_SIZE != 0)
            cursor = round_up_to_page(cursor);

        // Kick off prefetch for next page if we're past the midpoint of the current page
        maybe_prefetch_(page_number(cursor) + 1);

        return r;
    }

    // ...
};
```

The prefetch slots work as a 2-entry FIFO: when a prefetch completes, the page is handed to the read cache and to the
scanner's next call. If both slots are full and the scanner needs a third page ahead, it simply waits for the oldest
prefetch to complete.

### 4.12 Named / Persisted Iterators

Named iterators mirror FasterLog's `PersistedIterators` feature. A scanner can be registered with a string name; its
confirmed read position (`complete_until`) is included in every commit metadata write and restored on recovery.

```cpp
// Create a named scanner
Scanner* FastLog::scan(LogAddress begin, std::string_view name = "");

// Called by the consumer after successfully processing a record:
void Scanner::complete_until(LogAddress addr);
```

Internally, `FastLog` maintains:

```cpp
std::unordered_map<std::string, LogAddress> named_scanner_positions_;
```

On every commit, `named_scanner_positions_` is serialized into the commit metadata file alongside `flush_offset` and the
cookie. On recovery, `named_scanner_positions_` is populated from the last commit file.

When a named scanner is created with `recover = true` (the default), it starts from the recovered position rather than
`begin`. If no recovered position exists for the given name, it starts from `begin`.

**`complete_until` semantics**: the consumer calls `complete_until(addr)` only after it has fully processed the record
at `addr` and any downstream side effects are durable (e.g., written to a downstream queue, applied to a state machine).
The distinction between "cursor advanced past this record" and "consumer confirmed this record" is what enables
exactly-once semantics after a crash:

```
Consumer fiber:
  while true:
    Record r = co_await scanner.next()
    apply(r)                             // process the record
    co_await downstream.flush()          // make downstream effect durable
    scanner.complete_until(r.address)    // only now mark it consumed
    // if we crash here, on restart we will re-read r.address
    // and re-apply — but downstream.flush() must be idempotent
```

### 4.13 Truncation

```
void FastLog::truncate_until(LogAddress new_head):

  assert(new_head <= flush_offset_)    // can only truncate durable data
  assert(new_head % PAGE_SIZE == 0)    // must be page-aligned

  head_offset_ = new_head

  // delete segment files now fully below new_head
  for each segment S where (S + 1) * SEGMENT_SIZE <= new_head:
    co_await io_uring_unlink(segment_path(S))

  // evict read cache pages below new_head
  read_cache_.evict_below(new_head / PAGE_SIZE)
```

Truncation is cooperative: the caller chooses when to truncate. For a WAL, truncation happens after a checkpoint: once
all writes up to address `A` are reflected in durable B-tree state, WAL entries below `A` are no longer needed for
recovery.

### 4.14 Recovery

On restart, recovery proceeds in two steps: (1) load the last commit metadata, (2) verify and set cursors.

**Step 1 — load commit metadata**:

1. List all files in `log_commit/`, sorted by `commit_num` descending.
2. For each commit file (most recent first), attempt to deserialize a `CommitMetadata` record:
    - `flush_offset`, `head_offset`, `commit_num`, named scanner positions, cookie, checksum.
    - Verify checksum. If invalid, try the next file.
3. If no valid commit file is found, treat as a fresh log: `flush_offset = head_offset = 0`.

**Step 2 — verify log data**:
The last page on disk may be partially written (if the process crashed mid-flush). We verify the last page to find the
true `flush_offset`:

1. Starting from `commit_metadata.flush_offset - PAGE_SIZE` (the last committed page), read the page.
2. Scan records forward within the page using magic + CRC32C to find the last valid record.
3. If the full page looks valid, `flush_offset = commit_metadata.flush_offset` (the commit was fully persisted).
4. If the last page is partially corrupted, set `flush_offset` to the end of the last valid record in that page.
5. Set `tail_offset = flush_offset` (everything on disk is now immutable; nothing is in the mutable ring).
6. Set `head_offset = commit_metadata.head_offset`.
7. Restore `named_scanner_positions_` from `commit_metadata`.
8. Return `commit_metadata.cookie` to the application.

The application is responsible for replaying WAL entries from `head_offset` to `tail_offset` to rebuild in-memory state.

---

## 5. Integration with tinyio

Each `FastLog` instance is owned by exactly one `IO` (tinyio event loop). All methods must be called from fibers running
on that `IO`'s thread, or dispatched via `post()`.

Fiber layout within the owning IO thread:

```
IO thread (log owner)
├── [inline] append()
│     Writes to page buffer, returns LogAddress immediately.
│     No suspension, no I/O.
│
├── [background fiber] flusher_fiber()
│     Sleeps until a page is Sealed.
│     Issues io_uring writes, advances flush_offset, wakes commit_waiters.
│     Runs concurrently with other fibers on the same thread.
│
├── [background fiber] segment_allocator_fiber()
│     Preallocates the next segment file via io_uring fallocate.
│     Woken by flusher when approaching segment boundary.
│
├── [background fiber] commit_writer_fiber()
│     Triggered after flush_offset advances (or explicitly via commit()).
│     Serializes CommitMetadata, writes to log_commit/<commit_num>.
│     Advances committed_offset, wakes named_scanner checkpointing.
│
└── [on-demand, per caller] wait_until_flushed() / read() / Scanner::next()
      Fiber suspends on I/O or commit wait; resumes when condition satisfied.
      Multiple such fibers can be in-flight simultaneously.
```

The background fibers are started in `FastLog::open()` and stopped gracefully in `FastLog::close()` (drain all sealed
pages, write final commit, then exit).

Cross-thread append:

```cpp
// From a foreign thread, dispatch append to the log's owner:
LogAddress addr = co_await post(log_io, [&]{ return log.append(payload); });
co_await post(log_io, [&]{ return log.wait_until_flushed(addr); });
```

---

## 6. Configuration Reference

| Parameter                    | Type       | Default              | Description                                                                                      |
|------------------------------|------------|----------------------|--------------------------------------------------------------------------------------------------|
| `path`                       | `string`   | —                    | Directory for segment files and commit metadata.                                                 |
| `page_size`                  | `uint32_t` | `262144` (256 KB)    | Page size in bytes. Must be a power of two and ≥ 4096.                                           |
| `buffer_pages`               | `uint32_t` | `32`                 | In-memory ring buffer size in pages. Total = `page_size × buffer_pages`.                         |
| `segment_size`               | `uint64_t` | `536870912` (512 MB) | Segment file size. Must be a multiple of `page_size`.                                            |
| `cache_pages`                | `uint32_t` | `64`                 | SIEVE read cache size in pages (16 MB at default page size).                                     |
| `flush_mode`                 | `enum`     | `Relaxed`            | `Relaxed`: advance `flush_offset` on write completion. `Strict`: require `fdatasync`.            |
| `scan_buffering`             | `enum`     | `Double`             | `Single`: one prefetch buffer. `Double`: two buffers (prefetch next page while reading current). |
| `segment_prealloc_threshold` | `float`    | `0.8`                | Fraction of current segment used before preallocating the next.                                  |
| `auto_commit`                | `bool`     | `false`              | Automatically trigger a commit after each append (high durability, lower throughput).            |
| `remove_outdated_commits`    | `bool`     | `true`               | Delete commit metadata files superseded by a later successful commit.                            |

---

## 7. Open Questions

1. **Large record fragmentation**: the current design caps records at `PAGE_SIZE - 10`. For records larger than this
   (e.g., large value writes in a storage engine), options are: (a) multi-page records with a fragment chain in the
   header, (b) out-of-band large value store with the log holding a pointer. The pointer approach is simpler and avoids
   changing the scanner; fragmentation is more self-contained. Decision deferred.

2. **Fast commit mode (inline commit records)**: embedding commit records inline in the log stream (as FasterLog's
   `fastCommitMode` does) removes the separate `log_commit/` metadata write, cutting per-commit I/O by one. Cost is O
   (log size) recovery instead of O (1). Worth implementing as an opt-in mode once the basic design is validated.

3. **Durability mode granularity**: `Strict` flush mode issues one `fdatasync` per page. For high-throughput workloads
   this may be too conservative. An alternative is `fdatasync` per batch of N pages or per commit call rather than per
   page flush. Needs benchmarking.

4. **Commit writer decoupling**: currently the `commit_writer_fiber` is triggered by flush completion. An alternative is
   an explicit `commit()` call (like FasterLog) controlled by a `LogCommitPolicy`. The rate-limit policy is especially
   useful: "commit no more than once per 10ms or 1MB". This prevents thundering-herd commits under high write load.

5. **Named scanner wake after commit**: currently named scanners in committed mode wait on `commit_waiters_`. After
   recovery, the `commit_writer_fiber` must also wake named scanners whose `complete_until` position was restored from
   the commit file, so they do not re-wait for data they already confirmed.

6. **Segment file naming**: zero-padded decimal (`log_00000000.seg`) is human-readable but limits segment count to 10⁸.
   A hex scheme gives 16⁸ = ~4 billion segments. Decision: use zero-padded hex (`log_00000000.seg` where each digit is
   hex) for the segment number.

7. **Checksum algorithm**: CRC32C is hardware-accelerated and adequate. xxHash3 is faster for large payloads (>1KB) but
   not hardware-native. Decision: ship CRC32C, revisit if checksum appears in profiles.
