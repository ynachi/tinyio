# RFC-003: FastLog V1 - A Partitioned Durable Record Log

**Status:** Draft
**Date:** 2026-07-10
**Target:** Linux, C++23, single-thread-per-partition runtimes
**Primary implementation:** tinyio fibers and io_uring
**Format version:** 1

---

## Abstract

FastLog is a bounded-memory, append-only, durable record log. A FastLog instance
contains one or more independent partitions. Each partition has exactly one
writer, permits concurrent readers, and defines a total order over opaque
variable-length records.

The design separates four concerns that are often accidentally combined:

1. **Appending** places a complete record in the in-memory tail of the log.
2. **Flushing** writes log bytes to segment files so memory can be reused.
3. **Committing** publishes a durable checkpoint naming an exact recoverable prefix.
4. **Truncating** advances the logical beginning and later reclaims whole segments.

A successful commit guarantees prefix recovery. After a crash, recovery exposes
all records before the last acknowledged commit boundary and no records after it.
Data may have reached segment files without becoming committed; recovery ignores
such data.

This RFC specifies the public contract, state invariants, persistent format,
durability protocol, recovery algorithm, concurrency rules, and conformance test
suite. It intentionally does not make io_uring part of the correctness model.
The same protocol must be implementable by an in-memory test device, a POSIX
device, and an io_uring device.

---

## 1. Normative language

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHOULD**, **SHOULD NOT**,
and **MAY** are normative requirements.

Examples and pseudocode explain the requirements but do not override them.

---

## 2. Decision summary

FastLog V1 makes the following decisions:

- The unit visible to callers is an opaque record, not a byte stream and not a
  key-value operation.
- Ordering is total within one partition and undefined across partitions.
- Each partition has one owning execution thread and one logical writer.
- Record addresses are unsigned 64-bit logical byte positions.
- Addresses are stable until their records are truncated.
- Records do not cross logical page boundaries.
- Resident memory is a bounded circular set of page frames.
- Sealed pages are flushed eagerly; flushed pages remain useful as a RAM cache
  until their frames must be reused.
- A commit names an exact record boundary, never a block-aligned approximation.
- Commit metadata is stored in two alternating, checksummed checkpoint slots.
- The persistent wire format is explicitly little-endian and versioned.
- Per-record header and payload checksums are mandatory in V1.
- Reads copy data into caller-owned memory. Zero-copy leases are deferred.
- Default reads and scans expose committed records only.
- Truncation becomes crash-durable only when included in a successful commit.
- Segment deletion happens after the truncation checkpoint is durable.
- Multi-process writers, replication, compression, encryption, and transactions
  are out of scope for V1.

---

## 3. Goals and non-goals

### 3.1 Goals

FastLog V1 is designed to provide:

1. High-throughput sequential append.
2. Predictable memory usage.
3. Cooperative backpressure instead of unbounded allocation.
4. Batched durable commits.
5. Exact prefix recovery after process, kernel, or power failure.
6. Random reads by stable address.
7. Efficient sequential scans and durable tailing.
8. Head truncation and whole-segment reclamation.
9. Corruption detection before a stored length controls memory or I/O.
10. A storage protocol that can be tested without io_uring.
11. A format that can be evolved without relying on a compiler ABI.
12. Clear operational metrics and failure reporting.

### 3.2 Non-goals

FastLog V1 does not provide:

- Key lookup, upsert, delete, compare-and-swap, or read-modify-write.
- An embedded hash index.
- Record mutation after append.
- Cross-partition ordering or transactions.
- Multi-writer synchronization within a partition.
- Replication or consensus.
- Compression or encryption.
- Data repair after bit rot.
- Exactly-once application processing.
- Durable consumer offsets.
- Zero-copy record lifetimes.
- Records larger than one logical page.
- Automatic migration from unknown on-disk formats.

Applications can encode keys, tombstones, schemas, or event types inside the
opaque payload. Those meanings do not belong in the FastLog record header.

---

## 4. Failure model

The design covers these failures:

- The process exits or is killed at any instruction.
- The machine loses power.
- The kernel crashes.
- A write completes partially.
- A write, sync, open, close, rename, or unlink operation returns an error.
- The newest checkpoint slot is torn or corrupt.
- Uncommitted data exists in segment files after a crash.
- A segment contains corrupt record bytes.

The design assumes:

- The filesystem honors successful `fdatasync` or equivalent durability calls.
- Completed durability calls are ordered as observed by the process.
- Stable storage does not acknowledge data that it later silently discards.
- Atomic 4 KiB sector writes are **not** assumed.
- At least one previously successful checkpoint slot remains readable unless
  the storage device has suffered corruption beyond the stated model.
- One process owns a partition directory at a time.

Checksums detect corruption; they do not reconstruct lost data. Replication or
erasure coding would be a separate layer.

### 4.1 Durability boundary

The central rule is:

```text
Segment bytes becoming persistent does not make them recoverable.
Only a durable checkpoint makes a prefix recoverable.
```

The ordering is:

```text
    write data       sync data       write checkpoint       sync checkpoint
        |                |                   |                      |
        v                v                   v                      v
    +--------+       +--------+          +--------+             +--------+
    | dirty  | ----> | stable | -------> | named  | ----------> | durable|
    | bytes  |       | bytes  |          | prefix |             | commit |
    +--------+       +--------+          +--------+             +--------+

    <---------------- no acknowledgement -----------------------> ACK
```

FastLog MUST NOT acknowledge a commit before the last step succeeds.

---

## 5. Terminology

| Term | Meaning |
|------|---------|
| Partition | One independently ordered and persisted log. |
| Record | One opaque payload appended atomically to the logical sequence. |
| Logical address | Stable 64-bit byte position identifying a record header. |
| Frame | A record header, payload, and alignment padding. |
| Page | Fixed-size logical allocation and flush unit. |
| Page frame | One resident memory slot that temporarily represents a logical page. |
| Segment | File containing a fixed-size logical data region and a segment header. |
| Tail | Next logical allocation position. |
| Head | Oldest logical position still represented in resident memory. |
| Flush | Completion of data writes to segment files; not a durability promise. |
| Commit | Durable publication of a checkpoint naming a prefix. |
| Checkpoint | Small metadata record containing begin, committed tail, and generation. |
| Begin | Lowest address currently visible to readers. |
| Durable begin | Begin value in the latest durable checkpoint. |
| Padding frame | Internal frame that consumes unusable bytes at the end of a page. |
| Local visibility | Records appended by the current process, whether committed or not. |
| Committed visibility | Records named by the latest successful checkpoint. |

---

## 6. System architecture

### 6.1 Components

```text
 +-----------------------------------------------------------------------+
 |                             Application                               |
 +--------------------+----------------------+---------------------------+
                      |                      |
                 Append/Commit           Read/Scan
                      |                      |
                      v                      v
 +-----------------------------------------------------------------------+
 |                         Partition API                                 |
 | ownership checks, validation, visibility, lifecycle                   |
 +-------------+-----------------------+----------------------+----------+
               |                       |                      |
               v                       v                      v
 +------------------------+ +----------------------+ +-------------------+
 | Log Core               | | Commit Coordinator   | | Read Coordinator  |
 |                        | |                      | |                   |
 | - logical allocation   | | - group commit       | | - RAM lookup      |
 | - record framing       | | - commit waiters     | | - disk page-in    |
 | - page states          | | - checkpoint publish | | - scan prefetch   |
 | - address markers      | | - GC scheduling      | | - CRC validation  |
 +-----------+------------+ +----------+-----------+ +---------+---------+
             |                         |                       |
             +-------------------------+-----------------------+
                                       |
                                       v
 +-----------------------------------------------------------------------+
 |                         Storage interfaces                            |
 +--------------------------+----------------------+---------------------+
                            |                      |
                            v                      v
                 +-------------------+   +------------------------+
                 | SegmentStore      |   | CheckpointStore        |
                 |                   |   |                        |
                 | write/read/sync   |   | load/publish/sync      |
                 | create/delete     |   | two-slot validation    |
                 +---------+---------+   +-----------+------------+
                           |                         |
                           v                         v
                 +-------------------+   +------------------------+
                 | segment-*.flog    |   | checkpoint.flog       |
                 +-------------------+   +------------------------+
```

### 6.2 Separation of protocol and mechanism

The Log Core and Commit Coordinator define correctness. Storage backends define
how operations reach the kernel.

The following backends SHOULD exist:

```text
 +-----------------------+     deterministic faults, no kernel dependency
 | InMemorySegmentStore  | --> unit tests and model checking
 +-----------------------+

 +-----------------------+     pread/pwrite/fdatasync
 | PosixSegmentStore     | --> simple reference implementation
 +-----------------------+

 +-----------------------+     registered buffers/files, batched SQEs
 | IoUringSegmentStore   | --> optimized production implementation
 +-----------------------+
```

All backends MUST pass the same conformance suite.

### 6.3 Partitioning model

Each partition is independent:

```text
                    application partitioner
                              |
              +---------------+---------------+
              |               |               |
              v               v               v
        +-----------+   +-----------+   +-----------+
        | partition |   | partition |   | partition |
        |     0     |   |     1     |   |     2     |
        +-----+-----+   +-----+-----+   +-----+-----+
              |               |               |
           IO/core 0       IO/core 1       IO/core 2
```

The caller chooses the partition for each record. FastLog does not provide a
global sequencer. This preserves the thread-per-core execution model.

---

## 7. Logical address space

### 7.1 Address definition

```cpp
using LogAddress = std::uint64_t;
```

Address zero is valid. An API that needs an invalid address MUST use
`std::optional<LogAddress>` or a separate tagged type rather than reserving a
normal address.

An address identifies the first byte of a persistent record header in the
logical data space. It is not:

- a pointer,
- a record sequence number,
- a physical file offset,
- guaranteed to be adjacent to the previous record.

Addresses increase monotonically. FastLog MUST reject an append that would
overflow `uint64_t`.

Checkpoint generations are also monotonic `uint64_t` values. They MUST NOT wrap;
once the maximum generation is reached, further commits fail with
`generation_exhausted` and the log remains readable.

### 7.2 Address decomposition

The logical data region of every segment has the same power-of-two size.

```text
segment_id        = address / segment_data_size
offset_in_segment = address % segment_data_size

page_number       = address / page_size
offset_in_page    = address % page_size

physical_file_offset = segment_header_size + offset_in_segment
```

Example configuration:

```text
page_size           = 1 MiB
segment_data_size   = 1 GiB
segment_header_size = 4 KiB

logical address 1,073,745,920
    segment_id        = 1
    offset_in_segment = 4,000
    physical offset   = 4,096 + 4,000 = 8,096
```

The segment header is outside the logical address space. This keeps segment
mapping simple while retaining self-describing files.

### 7.3 Stable addresses and holes

Records do not cross page boundaries. When a record cannot fit in the current
page, the remaining bytes become padding and the record begins in the next page.

```text
Logical page N

 page start                                                   page end
     |                                                            |
     v                                                            v
     +-------------+----------------+-----------+-----------------+
     | record A    | record B       | record C  | padding         |
     +-------------+----------------+-----------+-----------------+
                                                  ^               ^
                                                  |               |
                                          no user record      next address

Logical page N+1

     +----------------------+--------------------------------------+
     | record D             | free                                 |
     +----------------------+--------------------------------------+
     ^
     |
 returned address for D
```

The address space is monotonic but not dense. Scanners understand padding;
callers never receive padding addresses.

---

## 8. State model and invariants

### 8.1 Address markers

FastLog tracks six logical positions:

```text
durable_begin   begin       head       committed_tail   flushed_tail    tail
      |           |           |               |               |          |
      v           v           v               v               v          v
------+-----------+-----------+---------------+---------------+----------+---->
      truncated       disk only       committed resident   uncommitted
      persistently    or resident      or disk             resident tail
```

The drawing is illustrative. `head` may lie on either side of
`committed_tail`: uncommitted but flushed pages may be evicted, while committed
pages may remain resident as cache.

The normative invariants are:

```text
durable_begin <= begin <= committed_tail <= flushed_tail <= tail
begin <= head <= flushed_tail
tail - head <= resident_capacity
```

Additional invariants:

1. `tail`, `flushed_tail`, and `committed_tail` are record boundaries.
2. `begin` and `durable_begin` are record boundaries or equal to a committed tail.
3. A record is published to local readers only after its header and payload are complete.
4. A page frame cannot be reused while an I/O operation references it.
5. A checkpoint cannot name bytes that have not completed data synchronization.
6. Segment deletion cannot precede the checkpoint that excludes the segment.

### 8.2 Storage states

```text
                                   process crash
                                        |
                                        v
 +----------------------+       +----------------------+
 | RAM only             |       | discarded            |
 | addr in [flushed,tail)| ----> | not recovered        |
 +----------+-----------+       +----------------------+
            |
            | successful write
            v
 +----------------------+       +----------------------+
 | Flushed, uncommitted |       | discarded            |
 | [committed,flushed)  | ----> | despite disk bytes   |
 +----------+-----------+ crash +----------------------+
            |
            | data sync + durable checkpoint
            v
 +----------------------+       +----------------------+
 | Committed            |       | recovered            |
 | [begin,committed)    | ----> | visible after reopen |
 +----------------------+ crash +----------------------+
```

### 8.3 Why flushed and committed are different

Flushing exists to manage memory. Committing exists to make an application
promise. A page may be flushed and evicted before it is committed. If a crash
occurs, the checkpoint tells recovery to ignore it.

This separation allows:

- eager sequential writes,
- bounded resident memory,
- group commit,
- an exact recovery boundary,
- no full log scan to discover the durable tail.

---

## 9. Configuration

V1 exposes quantities directly rather than encoding them as bit counts.

```cpp
struct FastLogSettings {
  std::filesystem::path directory;

  std::uint64_t page_size = 1_MiB;
  std::uint64_t segment_data_size = 1_GiB;
  std::uint32_t resident_pages = 64;
  std::uint32_t read_cache_pages = 32;
  std::uint32_t io_alignment = 4096;
  std::uint32_t max_payload_size = 1_MiB - 32;

  bool preallocate_segments = false;
  mode_t file_mode = 0644;
  mode_t directory_mode = 0755;
};
```

### 9.1 Required validation

Open MUST reject settings unless:

- `directory` is non-empty.
- `io_alignment` is a power of two and at least the device requirement.
- `page_size` is a power of two.
- `page_size` is a multiple of `io_alignment`.
- `segment_data_size` is a power of two.
- `segment_data_size` is a multiple of `page_size`.
- `resident_pages >= 3`.
- `max_payload_size + record_header_size <= page_size`.
- `page_size <= UINT32_MAX` because V1 frame lengths are 32-bit.
- `max_payload_size <= UINT32_MAX` because V1 payload lengths are 32-bit.
- all derived multiplications fit in `size_t`, `off_t`, and `uint64_t`.
- the configured fixed-buffer pool can supply the required resident and staging
  memory before the partition is considered open.

The on-disk checkpoint stores a geometry fingerprint. Reopening with different
geometry MUST fail unless an explicit migration tool is used.

### 9.2 Why there is no mutable fraction

V1 flushes naturally sealed pages eagerly. A flushed page can remain resident
and serve reads until its frame is needed for a newer logical page. Delaying
sealed-page writes with a mutable fraction increases commit bursts and makes
backpressure harder to reason about without improving the append contract.

An adaptive flush policy MAY be introduced later without changing the format or
durability protocol.

---

## 10. Directory and file layout

One partition owns one directory:

```text
partition-000000000000002a/
|
+-- LOCK
+-- checkpoint.flog
+-- segment-0000000000000000.flog
+-- segment-0000000000000001.flog
+-- segment-0000000000000002.flog
`-- ...
```

File names use fixed-width lowercase hexadecimal segment IDs. Temporary files,
if used, MUST have names that cannot be mistaken for segments.

The `LOCK` file protects against multiple process owners. Segment discovery MUST
parse complete names strictly; unrelated files are ignored but reported.

### 10.1 Segment file

```text
physical offset 0
      |
      v
+-------------------------------+  4 KiB
| SegmentHeaderV1               |
| magic/version/UUID/segment ID |
+-------------------------------+
|                               |
| Logical data region           |  segment_data_size bytes
|                               |
| records and padding frames    |
|                               |
+-------------------------------+
```

The physical file size MAY be smaller than the configured capacity until writes
extend it. Preallocation is a performance choice and MUST NOT be used to infer
the logical tail.

### 10.2 Segment header

The first 4 KiB contains:

| Field | Type | Meaning |
|-------|------|---------|
| magic | 8 bytes | `FASTSEG1` |
| major_version | u16 | Incompatible format version. |
| minor_version | u16 | Backward-compatible extension version. |
| header_length | u32 | Bytes understood by this version. |
| log_uuid | 16 bytes | Unique identity of the log. |
| partition_id | u64 | Application-visible partition identity. |
| segment_id | u64 | Monotonic segment number. |
| logical_base | u64 | `segment_id * segment_data_size`. |
| page_size | u64 | Geometry validation. |
| segment_data_size | u64 | Geometry validation. |
| created_generation | u64 | Checkpoint generation active at creation. |
| flags | u64 | Must-understand and optional feature flags. |
| header_crc32c | u32 | CRC with this field set to zero. |
| reserved | remaining | Must be zero in V1. |

The complete 4 KiB header is checksummed. Unknown must-understand flags cause
open to fail.

### 10.3 Segment creation protocol

Creating a segment is itself crash-sensitive:

```text
create file -> write segment header -> sync file -> sync directory -> use segment
```

A checkpoint MUST NOT reference records in a newly created segment until its
header and directory entry have completed this protocol.

---

## 11. Record wire format

### 11.1 RecordHeaderV1

Every user record and explicit padding frame begins with this 32-byte header:

```text
byte
 0              4       6       8              12
 +--------------+-------+-------+---------------+
 | magic        | ver   | flags | header CRC32C |
 +--------------+-------+-------+---------------+
 | payload CRC32C       | payload length        |
 +----------------------+-----------------------+
 | frame length         | logical address       |
 +----------------------+                       |
 |                                              |
 +----------------------------------------------+
 0                                             32
```

Normative field table:

| Field | Type | Description |
|-------|------|-------------|
| magic | 4 bytes | ASCII `FREC`, for fast rejection. |
| version | u16 | Record header version, initially 1. |
| flags | u16 | Frame kind and future optional features. |
| header_crc32c | u32 | CRC over the encoded header with this field zero. |
| payload_crc32c | u32 | CRC over exactly `payload_length` payload bytes. |
| payload_length | u32 | User payload bytes; zero is valid. |
| frame_length | u32 | Header, payload, and alignment/padding bytes. |
| logical_address | u64 | Address at which this header is stored. |

All integers are little-endian. Encoding and decoding MUST use explicit helper
functions. Persisting a C++ struct by `write(sizeof(struct))` is forbidden.

V1 uses CRC-32C (Castagnoli) with the reflected polynomial `0x82F63B78`, an
initial value of all ones, and a final XOR of all ones. CRC values are encoded as
little-endian `u32`. The header CRC is calculated over the complete encoded
32-byte header with `header_crc32c` set to zero.

### 11.2 Flags

V1 defines the low two bits as a frame kind:

```text
00 = user data
01 = page padding
10 = reserved
11 = reserved
```

All other bits MUST be zero in V1. Readers reject unknown must-understand bits.

### 11.3 Frame length

User frames are aligned to 8 bytes:

```text
frame_length = align_up(32 + payload_length, 8)
```

Layout:

```text
+----------------------+--------------------------+----------------+
| 32-byte header       | payload                  | zero padding   |
+----------------------+--------------------------+----------------+
^                                                 ^                ^
logical address                             payload end       next frame
```

The alignment padding MUST be zero. It is not included in the payload checksum.

### 11.4 Header-first validation

The header CRC is separate from the payload CRC so a reader can validate
`payload_length` and `frame_length` before allocating memory or issuing the
payload read.

Validation order is mandatory:

1. Read exactly 32 bytes.
2. Validate magic and supported version.
3. Validate the header CRC.
4. Validate frame kind.
5. Validate `logical_address` equals the requested address.
6. Validate lengths and page bounds.
7. Read the payload.
8. Validate the payload CRC.

### 11.5 Padding frames

If at least 32 bytes remain in a page, a padding header consumes the remainder:

```text
page end
   |
   v
+----------------------+----------------------------------+
| Padding header       | zero bytes                       |
| payload_length = 0   |                                  |
| frame_length = rest  |                                  |
+----------------------+----------------------------------+
```

If fewer than 32 bytes remain, those bytes are zeroed and the scanner jumps to
the next page because no complete header can fit.

Padding frames are internal and are never returned by reads or scans.

### 11.6 Maximum record size

The full frame MUST fit in one page:

```text
align_up(32 + payload_length, 8) <= page_size
```

Oversized appends fail with `record_too_large`; they are never split implicitly.
A future large-object facility can store an external reference as the payload.

---

## 12. Checkpoint format

### 12.1 Two-slot file

`checkpoint.flog` contains two independent 4 KiB slots:

```text
offset 0                                                    offset 8192
   |                                                             |
   v                                                             v
   +----------------------------+-------------------------------+
   | slot 0, generation N       | slot 1, generation N+1        |
   +----------------------------+-------------------------------+
            4096 bytes                    4096 bytes
```

Commits alternate slots:

```text
generation 0 -> slot 0, created with the log
generation 1 -> slot 1
generation 2 -> slot 0
generation 3 -> slot 1
```

While generation 2 is being written, generation 1 remains untouched and valid.
No atomic-sector-write assumption is required.

### 12.2 CheckpointSlotV1

Each slot contains:

| Field | Type | Meaning |
|-------|------|---------|
| magic | 8 bytes | `FASTCKP1` |
| major_version | u16 | Checkpoint format major. |
| minor_version | u16 | Checkpoint format minor. |
| slot_length | u32 | Encoded slot size. |
| log_uuid | 16 bytes | Must match segment headers. |
| partition_id | u64 | Must match the opened partition. |
| generation | u64 | Strictly increasing commit generation. |
| durable_begin | u64 | Lowest address recovered after crash. |
| committed_tail | u64 | Exclusive end of recoverable prefix. |
| page_size | u64 | Persistent geometry. |
| segment_data_size | u64 | Persistent geometry. |
| feature_flags | u64 | Enabled format features. |
| configuration_hash | u64 | Hash of persistent settings. |
| slot_crc32c | u32 | CRC over all 4 KiB with this field zero. |
| reserved | remaining | Must be zero in V1. |

The checksum covers the entire slot, including zeroed reserved bytes.

`configuration_hash` is XXH3-64 over this canonical little-endian sequence:

```text
page_size || segment_data_size || record_header_size || feature_flags
```

It is a compatibility fingerprint, not an integrity mechanism; the slot CRC
provides integrity.

### 12.3 Selecting the latest checkpoint

Recovery validates both slots independently:

```text
              +--------------------+
              | read both slots    |
              +---------+----------+
                        |
             +----------+----------+
             |                     |
             v                     v
       validate slot 0       validate slot 1
             |                     |
             +----------+----------+
                        |
                        v
          choose highest valid generation
```

Cases:

| Slot 0 | Slot 1 | Result |
|--------|--------|--------|
| invalid | invalid | Empty new log only if the directory has no segments; otherwise corruption. |
| valid | invalid | Recover slot 0. |
| invalid | valid | Recover slot 1. |
| valid | valid | Recover the higher generation. |
| same generation | same generation | Accept only if contents are identical. |
| same generation | different data | Report corruption. |

---

## 13. Resident memory architecture

### 13.1 Circular page frames

Resident memory is a fixed array of page-sized frames:

```text
logical pages increase forever

    0   1   2   3   4   5   6   7   8   9  10 ...
    |   |   |   |   |   |   |   |   |   |   |
    v   v   v   v   v   v   v   v   v   v   v

resident frame ring, example size 4

  +---------+---------+---------+---------+
  | frame 0 | frame 1 | frame 2 | frame 3 |
  +---------+---------+---------+---------+
      ^         ^         ^         ^
      |         |         |         |
   pages      pages     pages     pages
  0,4,8...   1,5,9... 2,6,10... 3,7,11...
```

Mapping:

```text
frame_index = page_number % resident_pages
```

Every frame stores its current logical page number. A mapping is valid only if
the tag matches. This prevents stale circular-buffer data from being mistaken
for the requested address.

### 13.2 Page frame metadata

```cpp
enum class PageState {
  Free,
  Mutable,
  Sealed,
  Flushing,
  Flushed,
  Loading,
};

struct PageFrameMeta {
  std::uint64_t logical_page;
  PageState state;
  std::uint32_t used_bytes;
  std::uint32_t io_references;
};
```

The byte storage is owned by a registered-buffer arena in the io_uring backend.
Metadata is not persisted.

### 13.3 Page lifecycle

```text
                       append fills page
                              |
                              v
 +------+    claim     +-----------+    seal    +--------+
 | Free | -----------> | Mutable   | ---------> | Sealed |
 +------+              +-----------+            +---+----+
    ^                                               |
    |                                               | submit write
    |                                               v
    |                                           +----------+
    |                                           | Flushing |
    |                                           +----+-----+
    |                                                |
    |                                  successful write completion
    |                                                |
    |                                                v
    |                                           +---------+
    +-------------------------------------------| Flushed |
                 reuse when needed             +---------+
```

Failure transitions:

```text
Sealed --submission failure--> Sealed + partition write error
Flushing --I/O failure-------> Sealed + partition write error
```

A failed frame is not reused. The partition remains readable but appends may
eventually block until the error is resolved or the partition is closed.

### 13.4 Resident window example

```text
Logical address space

0                 head                         tail
|                   |                            |
v                   v                            v
+-------------------+----------------------------+---------------->
| disk only         | resident circular window   | unallocated
+-------------------+----------------------------+---------------->

Resident memory

+----------+----------+----------+----------+
| flushed  | flushed  | sealed   | mutable  |
| page 40  | page 41  | page 42  | page 43  |
+----------+----------+----------+----------+
    oldest                                newest
```

Flushed pages remain resident until their frames are required by newer pages.
This gives recent reads a cache without delaying writes.

### 13.5 Backpressure

The writer cannot claim a frame that is mutable, sealed, flushing, loading, or
otherwise referenced.

```text
append needs next frame
          |
          v
 +------------------+
 | frame reusable?  |
 +----+---------+---+
      | yes     | no
      v         v
   continue   TryAppend: return EAGAIN
              Append: park fiber
                         |
                         v
                 flush completion wakes it
```

No busy waiting is permitted on the owning event-loop thread.

---

## 14. Public API

The exact C++ spelling may evolve, but V1 semantics are represented by:

```cpp
class FastLogPartition {
 public:
  static Result<std::unique_ptr<FastLogPartition>> Open(
      FastLogSettings settings,
      PartitionId partition_id,
      tinyio::IO& io);

  Result<LogAddress> TryAppend(std::span<const std::byte> payload);
  Result<LogAddress> Append(std::span<const std::byte> payload);

  Result<std::size_t> ReadInto(
      LogAddress address,
      std::span<std::byte> destination,
      Visibility visibility = Visibility::Committed);

  Result<Record> Read(
      LogAddress address,
      Visibility visibility = Visibility::Committed);

  Result<Scanner> Scan(ScanOptions options);

  Result<CommitInfo> Commit();
  Result<void> WaitCommitted(LogAddress exclusive_end);

  Result<void> TruncateUntil(LogAddress address);
  Result<void> Close(CloseMode mode = CloseMode::NoCommit);

  LogAddress Begin() const;
  LogAddress Tail() const;
  LogAddress FlushedTail() const;
  LogAddress CommittedTail() const;
};
```

### 14.1 Append result

The returned address identifies the record header. Append success means the
record is complete and locally readable. It does not mean flushed or committed.

### 14.2 Visibility

```cpp
enum class Visibility {
  Committed,
  Local,
};
```

- `Committed` accepts addresses in `[begin, committed_tail)`.
- `Local` accepts addresses in `[begin, tail)` in the current process.

Defaulting to committed visibility prevents callers from accidentally treating
volatile records as durable.

### 14.3 Commit result

```cpp
struct CommitInfo {
  std::uint64_t generation;
  LogAddress begin;
  LogAddress committed_tail;
};
```

The returned tail is exact. If `Commit()` is called with no new records, it still
MUST publish a checkpoint when `begin` has changed. Otherwise it MAY return the
current `CommitInfo` without I/O.

### 14.4 Close modes

```cpp
enum class CloseMode {
  NoCommit,       // drain submitted I/O, preserve last successful checkpoint
  CommitPending,  // commit the current tail, then close
};
```

Destruction MUST NOT silently initiate asynchronous durability work. `Close`
drains operations explicitly. A destructor may perform synchronous safety-net
cleanup only after no operation can reference the arena.

---

## 15. Append protocol

### 15.1 Fast path

Append is non-yielding after capacity has been obtained:

```text
caller payload
      |
      v
+-------------------+
| validate size     |
+---------+---------+
          |
          v
+-------------------+     no room in page     +-------------------+
| inspect mutable   | ----------------------> | write padding     |
| page capacity     |                         | seal old page     |
+---------+---------+                         +---------+---------+
          | room                                        |
          |                                             v
          |                                  obtain next free frame
          |                                             |
          +---------------------+-----------------------+
                                |
                                v
                    +--------------------------+
                    | encode header in scratch |
                    | copy payload             |
                    | compute payload CRC      |
                    | compute header CRC       |
                    +------------+-------------+
                                 |
                                 v
                    +--------------------------+
                    | publish new tail         |
                    +------------+-------------+
                                 |
                                 v
                         return record address
```

### 15.2 Publication order

The writer MUST construct the complete frame before advancing `tail`.

```text
old tail                                               new tail
   |                                                      |
   v                                                      v
---+------------------------------------------------------+---->
   | header | payload | alignment padding                |
---+------------------------------------------------------+---->
       1         2              3                 4: publish tail
```

Because one owning thread executes appends and append does not yield during
construction, no atomics are required for same-thread local readers.

### 15.3 TryAppend versus Append

- `TryAppend` MUST NOT yield. It returns `would_block` without modifying state.
- `Append` MAY yield while waiting for a reusable frame.
- Both return `record_too_large` for an oversized payload.
- Both return a persistent partition error if prior flush failure prevents progress.

---

## 16. Flush protocol

### 16.1 Eager sealed-page flushing

When a page becomes sealed, FastLog queues it for writing. Multiple adjacent
sealed pages SHOULD be coalesced into one storage operation when alignment and
segment boundaries allow it.

```text
page 100       page 101       page 102       page 103
+----------+   +----------+   +----------+   +----------+
| sealed   |   | sealed   |   | sealed   |   | mutable  |
+-----+----+   +-----+----+   +-----+----+   +----------+
      |              |              |
      +--------------+--------------+
                     |
                     v
             one coalesced write
```

Writes MUST split at segment boundaries.

### 16.2 Completion and contiguous advancement

Writes may complete out of order, but `flushed_tail` advances only across a
contiguous completed prefix.

```text
submission order:  page 10   page 11   page 12
completion state:  complete  pending   complete

flushed_tail may advance through page 10 only.
page 12 completion is remembered until page 11 completes.
```

### 16.3 Exact writes

A storage operation returning fewer bytes than requested is a short write. It
MUST NOT be treated as successful completion of the page. The backend may retry
the remainder or return an error; the Log Core sees completion only after the
entire requested range is written.

### 16.4 Eviction

A flushed frame becomes reusable when:

- its write has completed,
- no read or scan references it,
- no commit staging operation references it,
- it is the frame required for a newer logical page.

Eviction advances `head` over a contiguous sequence of non-resident pages.

---

## 17. Commit protocol

### 17.1 Commit meaning

`Commit()` snapshots the current exact tail and does not include records appended
after that snapshot.

```text
records:  A       B       C       D       E
          +-------+-------+-------+-------+
                          ^               ^
                          |               |
                    commit target      later tail

The commit acknowledges A, B, and C only.
```

### 17.2 Commit state machine

```text
 +-------+
 | Idle  |
 +---+---+
     | Commit request
     v
 +------------------+
 | Snapshot target  |
 +--------+---------+
          |
          v
 +------------------+
 | Flush through    |
 | exact target     |
 +--------+---------+
          |
          v
 +------------------+
 | Sync touched     |
 | segments         |
 +--------+---------+
          |
          v
 +------------------+
 | Publish alternate|
 | checkpoint slot  |
 +--------+---------+
          |
          v
 +------------------+
 | Sync checkpoint  |
 +--------+---------+
          |
          v
 +------------------+
 | Advance committed|
 | wake waiters     |
 +--------+---------+
          |
          v
 +------------------+
 | Schedule GC      |
 +--------+---------+
          |
          v
        Idle
```

### 17.3 Detailed algorithm

For target `T` and pending begin `B`:

1. Validate partition health and lifecycle state.
2. Snapshot `T = tail`, `B = begin`, and `G = generation + 1`.
3. Ensure every naturally sealed page below `T` has completed writing.
4. Snapshot any partial mutable-page bytes needed to reach `T` into registered,
   aligned staging memory.
5. Zero staging bytes after `T` up to the storage alignment boundary.
6. Write every range needed to make `[flushed_tail, T)` present in segments.
7. Verify exact write lengths.
8. Synchronize every segment dirtied since the previous commit.
9. Ensure newly created segment directory entries are durable.
10. Encode checkpoint `{G, B, T, geometry}` into the alternate slot.
11. Write the complete checkpoint slot.
12. Synchronize the checkpoint file.
13. Set `generation = G`, `durable_begin = B`, and `committed_tail = T`.
14. Wake all waiters whose requested boundary is `<= T`.
15. Queue deletion of segments entirely below `durable_begin`.

Any failure before step 13 leaves the in-memory committed markers unchanged.
Flushed bytes may remain and can be reused by a retry.

### 17.4 Partial-page staging

Direct I/O cannot safely read from a mutable page while another fiber appends to
the same aligned block. Commit therefore snapshots the affected aligned range.

```text
mutable page in arena

+----------------------+------------------+------------------------+
| older bytes          | commit prefix    | future/free bytes      |
+----------------------+------------------+------------------------+
                       ^                  ^
                       block start        target T

registered staging buffer

+----------------------+------------------+------------------------+
| copied older bytes   | copied prefix    | zero                   |
+----------------------+------------------+------------------------+
^                                                                ^
aligned write start                                      aligned write end
```

After the copy is complete, appends may continue in the mutable page while the
staging write is in flight.

### 17.5 Segment synchronization

A commit may cross several segments:

```text
segment 7             segment 8             segment 9
+------------------+  +------------------+  +------------------+
| dirty bytes      |  | dirty bytes      |  | target prefix    |
+------------------+  +------------------+  +------------------+
       |                    |                     |
       v                    v                     v
   fdatasync(7)         fdatasync(8)          fdatasync(9)
       +--------------------+---------------------+
                            |
                            v
                    checkpoint publication
```

All required segment syncs MUST succeed before checkpoint publication.

### 17.6 Group commit

Only one physical commit is active. Requests are coalesced:

```text
time -------------------------------------------------------------->

request A target 100 ----+
request B target 100 -----+--> physical commit target 100 --> wake A,B

                    request C target 160 ----+
                    request D target 220 ----+--> next commit target 220
```

Every logical request snapshots two values:

```text
requested committed tail
requested durable begin
```

An active physical commit covers a request only when its checkpoint has:

```text
checkpoint.committed_tail >= request.requested_tail
checkpoint.durable_begin  >= request.requested_begin
```

This matters when truncation advances begin while a commit with the same tail is
already active. The newer truncation request must wait for a later checkpoint.
A request arriving after the active snapshot can join that commit only when both
of its requested boundaries are covered.

### 17.7 WaitCommitted

`WaitCommitted(X)`:

- returns immediately when `committed_tail >= X`,
- otherwise parks the current fiber,
- wakes on a covering successful commit,
- returns the relevant commit error if progress fails,
- does not itself issue a commit.

This separation lets an application run a periodic group-commit fiber.

---

## 18. Read protocol

### 18.1 Location decision

```text
requested address
        |
        v
+-------------------+
| visibility bounds |
+---------+---------+
          |
          v
+-------------------+      tagged page resident
| locate page       | ------------------------------+
+---------+---------+                                |
          | not resident                             v
          v                                  +---------------+
+-------------------+                         | read from RAM |
| read/cache page   |                         +---------------+
| from segment      |
+---------+---------+
          |
          v
 validate header, bounds, payload CRC
```

### 18.2 Bounds

For committed visibility:

```text
begin <= address < committed_tail
```

For local visibility:

```text
begin <= address < tail
```

These checks reject only obvious out-of-range addresses. The record header's
magic, address, lengths, and checksums prove that the address is a record start.

### 18.3 Safe decoding

After header CRC validation, the reader MUST verify:

- frame kind is user data,
- `payload_length <= max_payload_size`,
- `frame_length == align_up(32 + payload_length, 8)`,
- `frame_length <= page_size`,
- the frame does not cross its page boundary,
- the frame end does not exceed the selected visibility tail,
- the encoded logical address equals the requested address.

Only then may it allocate or read the payload.

### 18.4 Buffer-too-small behavior

`ReadInto` returns `buffer_too_small` with the required payload size. It does not
partially copy a record. `Read` may allocate an owned buffer after validated
length checks.

### 18.5 Read cache

The read cache stores logical pages, not merely file descriptors:

```text
address -> logical page number -> cache lookup
                                |
                     +----------+----------+
                     | hit                 | miss
                     v                     v
                validate record      load aligned page
                                      into cache frame
```

The cache key includes log UUID and logical page number. A truncated page is
removed or made unreachable by bounds checks.

---

## 19. Scan protocol

### 19.1 Snapshot scan

A normal scan snapshots its end boundary at creation:

```text
Scan(begin = A, end = committed_tail_at_creation)
```

Later commits do not extend that scanner.

### 19.2 Durable tailing scan

A tailing scanner follows `committed_tail` only:

```text
read committed records -> reach committed tail -> park
        ^                                           |
        |                                           |
        +------------ next successful commit ------+
```

It never emits locally appended but uncommitted records unless explicitly
constructed with local visibility.

### 19.3 Page traversal

```text
current address
      |
      v
remaining bytes < header size? --yes--> next page
      |
      no
      v
decode header
      |
      +-- user data --> return record, advance by frame_length
      |
      +-- padding ---> next page
      |
      `-- invalid ----> corruption error
```

Scanners SHOULD prefetch sequential pages and release old cache references
promptly so slow consumers do not pin the writer's resident ring.

### 19.4 Truncation interaction

A scanner does not pin logical retention indefinitely. If truncation advances
past its next address, the scanner returns `truncated`. Applications requiring
consumer retention must coordinate truncation externally.

---

## 20. Truncation and garbage collection

### 20.1 Logical truncation

`TruncateUntil(A)` changes the in-memory `begin` immediately but does not delete
files and is not crash-durable until a later commit.

Requirements:

- `A` is a known record boundary or equals `committed_tail`.
- `begin <= A <= committed_tail`.
- truncation is monotonic.
- reads below the new begin fail immediately.

Before commit:

```text
durable_begin             begin
      |                     |
      v                     v
------+---------------------+------------------------------>
      crash restores here   current process hides this range
```

After commit:

```text
durable_begin == begin
             |
             v
-------------+--------------------------------------------->
             crash also restores here
```

### 20.2 Deletion ordering

```text
advance begin
     |
     v
commit checkpoint containing new begin
     |
     v
checkpoint sync succeeds
     |
     v
delete whole segments below durable begin
```

Deleting first is forbidden because a crash could restore an older checkpoint
that still requires the deleted segment.

### 20.3 Segment eligibility

A segment is reclaimable when:

```text
segment_logical_end <= durable_begin
```

The segment containing `durable_begin` remains, even when its prefix is dead.
V1 does not compact partial segments.

### 20.4 Failed deletion

Deletion failure does not invalidate the logical commit. It produces an
operational error and remains in a retry queue. The deletion cursor advances
only after successful unlink. Directory synchronization SHOULD follow deletion
to make physical reclamation durable.

### 20.5 Open readers

An in-flight reader may hold a segment handle while the path is unlinked. The
handle remains valid until the read completes. Descriptor ownership MUST be
reference-counted or otherwise pinned across yielding reads.

---

## 21. Recovery protocol

### 21.1 Open state machine

```text
+------------------+
| validate settings|
+--------+---------+
         |
         v
+------------------+
| ensure directory |
+--------+---------+
         |
         v
+------------------+    lock held elsewhere    +------------------+
| acquire LOCK     | ------------------------> | already_open     |
+--------+---------+                           +------------------+
         |
         v
+------------------+
| read checkpoints |
+--------+---------+
         |
    +----+--------------------+
    |                         |
    v                         v
no valid slots            latest valid slot
    |                         |
    v                         v
empty directory?       validate UUID/geometry
    |                         |
 +--+--+                      v
 | yes | new log        validate required segments
 +-----+                      |
 | no                         v
 v                      rebuild tail page
corruption                   |
                             v
                           Open
```

### 21.2 Fresh log creation

A fresh log creates:

1. the partition directory,
2. the lock file,
3. a new random log UUID,
4. the checkpoint file with a valid generation-zero checkpoint in slot 0 and a
   zeroed slot 1,
5. segment zero with a valid segment header.

The segment header, generation-zero checkpoint, files, and required directory
entries are synchronized before `Open` succeeds. Therefore, a successfully
created empty log is always distinguishable from an interrupted creation.
The initial logical state is:

```text
durable_begin = begin = head = committed_tail = flushed_tail = tail = 0
generation = 0
```

### 21.3 Existing log recovery

Given checkpoint `{generation G, begin B, tail T}`:

1. Validate all fields and arithmetic.
2. Validate `B <= T` and both are legal boundaries.
3. Validate every segment required by `[B, T)` exists and has the expected header.
4. Set `durable_begin = begin = B`.
5. Set `committed_tail = flushed_tail = tail = T`.
6. Rebuild the mutable tail page.
7. Initialize resident `head` to the first recovered resident page or `T`.
8. Queue stale segments below `B` for garbage collection.
9. Ignore all segment bytes at addresses `>= T`.

### 21.4 Rebuilding the tail page

If `T` is page-aligned, recovery claims a clean page for the next append.

If `T` is inside a page:

```text
segment page on disk

+-----------------------------+------------------------------+
| committed prefix            | stale uncommitted bytes      |
+-----------------------------+------------------------------+
                              ^
                              T

recovered mutable frame

+-----------------------------+------------------------------+
| read committed prefix       | zero                         |
+-----------------------------+------------------------------+
```

Recovery reads the aligned prefix, validates frames up to `T`, zeroes everything
after `T`, tags the frame with its logical page number, and marks it mutable.

### 21.5 Recovery validation scope

Fast open MUST validate:

- checkpoint checksums and geometry,
- segment presence and headers,
- the tail page up to the committed boundary.

It MAY defer full historical payload checksum validation until records are read
or scanned. A separate offline verification tool SHOULD scan all retained data.

### 21.6 Uncommitted bytes after crash

Uncommitted bytes may be complete and checksummed. They are still ignored:

```text
checkpoint tail T
      |
      v
+-----+----------------------+-------------------------------+
| committed records         | valid-looking uncommitted data|
+----------------------------+-------------------------------+
                             ignored, even if CRC is valid
```

The checkpoint, not record validity, defines recovery visibility.

---

## 22. Crash-point analysis

For one commit generation:

| Crash point | Disk state | Recovery result |
|-------------|------------|-----------------|
| Before any data write | Old checkpoint, old data prefix | Previous commit. |
| During data write | Partial new bytes | Previous commit; partial bytes ignored. |
| After data write, before data sync | New bytes may be volatile | Previous commit. |
| After data sync, before checkpoint write | Stable new bytes, old checkpoint | Previous commit. |
| During checkpoint slot write | New slot may be torn | Previous valid slot. |
| After slot write, before checkpoint sync | New slot not guaranteed durable | Previous or new valid generation; never an invalid prefix. |
| After checkpoint sync, before in-memory publish | New checkpoint durable | Recovery observes new commit even if caller did not receive ACK. |
| After ACK | New checkpoint durable | New commit. |
| During post-commit segment deletion | New begin durable | New commit; extra old files may remain. |

The unavoidable interval after durable checkpoint and before caller ACK can
produce a durable commit whose success was not observed. Applications requiring
idempotence must include their own record identity.

---

## 23. Concurrency and fiber semantics

### 23.1 Ownership

Each partition is owned by one `tinyio::IO` thread. Direct method calls from
another thread fail an ownership assertion or return `wrong_executor`. Remote
callers use `IO::Post` or an application message queue.

### 23.2 Cooperative concurrency

Single-threaded does not mean non-concurrent. Fibers interleave whenever an
operation yields:

```text
fiber A: Commit -> submit write -> yield -------------------> resume
                                      |
fiber B:                         Read / Append / Commit request
```

All methods that yield MUST leave shared state in a documented state before
suspension.

### 23.3 Writer rules

- Frame construction is non-yielding.
- Tail publication occurs after construction.
- There is one mutable page.
- A pending physical commit is owned by the Commit Coordinator.
- A second commit request joins or queues; it never mutates active commit state.
- Truncation updates pending begin but cannot alter the snapshot of an active commit.

### 23.4 Read rules

- RAM reads copy without yielding once the frame is validated.
- Disk reads pin their segment handle and cache frame across suspension.
- A scanner pins at most a bounded number of pages.
- Garbage collection may unlink a path but cannot invalidate a pinned handle.

### 23.5 Shutdown rules

Close transitions:

```text
Open -> Closing -> Closed
```

During `Closing`:

- new appends, scans, and commits are rejected,
- existing reads may drain,
- submitted writes are completed or cancelled and observed,
- registered buffers are not returned until the kernel no longer references them,
- file handles are closed after their operations finish.

---

## 24. Storage interfaces

The protocol depends on explicit storage semantics rather than syscalls:

```cpp
class SegmentStore {
 public:
  virtual Result<void> CreateSegment(SegmentId, const SegmentHeader&) = 0;
  virtual Result<void> WriteExact(LogAddress, ByteSpan) = 0;
  virtual Result<void> ReadExact(LogAddress, MutableByteSpan) = 0;
  virtual Result<void> SyncSegments(std::span<const SegmentId>) = 0;
  virtual Result<void> RemoveSegment(SegmentId) = 0;
  virtual Result<void> SyncDirectory() = 0;
};

class CheckpointStore {
 public:
  virtual Result<CheckpointSelection> LoadLatest() = 0;
  virtual Result<void> WriteSlot(std::uint32_t slot,
                                 const CheckpointSlot&) = 0;
  virtual Result<void> Sync() = 0;
};
```

`WriteExact` either writes the entire range or returns an error. It does not
expose a successful short write to the Log Core.

`WriteSlot` has the same exact-write requirement. A checkpoint generation is not
durable until the following `Sync` succeeds.

### 24.1 Fault-injection backend

The in-memory backend can fail before or after any logical operation:

```text
CreateSegment
WriteExact
SyncSegments
PublishCheckpointWrite
SyncCheckpoint
RemoveSegment
SyncDirectory
```

It also models torn writes by persisting only a prefix. Tests restart the Log
Core against the resulting byte arrays.

### 24.2 POSIX backend

The reference backend uses loops around `pread` and `pwrite`, handles `EINTR`,
checks exact lengths, and calls `fdatasync`. It favors clarity over maximum
throughput and acts as an executable specification.

### 24.3 io_uring backend

The production backend MAY use:

- registered fixed buffers for resident and staging pages,
- registered segment descriptors,
- linked SQEs for ordered commit barriers,
- batched adjacent page writes,
- completion-driven fiber wakeups,
- provided buffer rings or a fixed read cache for page-in.

The optimization MUST preserve the same completion and durability boundaries as
the reference backend.

### 24.4 Linked commit example

```text
WRITE_FIXED data pages
        |
        v completion observed
FSYNC segment 7 --link--> FSYNC segment 8 --link--> WRITE checkpoint --link--> FSYNC checkpoint
                                                                          |
                                                                          v
                                                                  one terminal CQE
```

If any linked operation fails, the chain fails and `committed_tail` does not
advance.

### 24.5 Registered-buffer lifetime

```text
claim frame -> submit I/O -> kernel owns pointer -> CQE observed -> frame reusable
```

Returning a fixed buffer to the pool before its terminal completion is a
use-after-free from the kernel's perspective and is forbidden.

---

## 25. Direct I/O requirements

An O_DIRECT backend MUST discover or conservatively configure the device's
alignment requirements. It MUST align:

- memory addresses,
- file offsets,
- write and read lengths.

### 25.1 Full pages

Full logical pages are naturally aligned and can be written directly from the
resident arena.

### 25.2 Partial commit

The commit staging buffer rounds the physical write outward while retaining the
exact logical checkpoint tail:

```text
logical target:                         T
                                        |
                                        v
physical write: [align_down(start) ---------------- align_up(T)]
checkpoint tail: ----------------------------------- T
```

Physical zero padding after `T` does not become part of the committed logical
prefix.

### 25.3 Buffered reads versus direct writes

Mixing buffered and direct I/O to overlapping ranges has filesystem-specific
performance and coherency costs. The production backend SHOULD use aligned
page-in for disk reads unless a tested platform profile explicitly permits a
buffered read descriptor.

This is a backend decision, not a format decision.

---

## 26. Error model

Errors are structured and preserve the operation and system error where relevant.

Suggested categories:

| Error | Meaning |
|-------|---------|
| invalid_settings | Geometry or required resource is invalid. |
| already_open | Another process owns the partition lock. |
| wrong_executor | Method invoked from a non-owning thread. |
| record_too_large | Frame cannot fit in one page. |
| address_overflow | Append would exhaust the 64-bit logical address space. |
| generation_exhausted | Checkpoint generation reached its maximum value. |
| would_block | TryAppend cannot obtain capacity immediately. |
| out_of_range | Address outside selected visibility bounds. |
| not_record_start | Header/address validation failed. |
| buffer_too_small | Destination cannot contain the whole payload. |
| truncated | Requested record or scan position is below begin. |
| corruption | Checksum, format, geometry, or segment validation failed. |
| unsupported_version | Persistent major version is unknown. |
| io_error | Storage operation failed. |
| short_io | Backend could not complete an exact read or write. |
| commit_failed | Data or checkpoint durability sequence failed. |
| closing | Partition is no longer accepting new operations. |

Append capacity exhaustion is not an I/O error. Corruption is not reported as
not-found. These distinctions matter operationally.

### 26.1 Error state after flush failure

A flush failure records the failed range and error. Reads of earlier valid data
continue. Appends may continue only while they do not require reuse of the
failed frame. Commit returns the failure until the range is successfully retried
or the partition is reopened.

---

## 27. File descriptor and cache ownership

Segment descriptors are owned by a segment manager:

```text
                         +----------------------+
read request ----------> | acquire shared lease |
                         +----------+-----------+
                                    |
                                    v
                              yielding read
                                    |
                                    v
                               release lease

GC request ------------> remove cache entry -> unlink path
                              |
                              v
                    close after final lease ends
```

The descriptor cache capacity is measured in open segments. The page cache
capacity is measured in logical pages. They are separate settings and metrics.

---

## 28. Observability

Each partition SHOULD expose counters and gauges.

### 28.1 Address gauges

- `fastlog_begin_address`
- `fastlog_durable_begin_address`
- `fastlog_head_address`
- `fastlog_tail_address`
- `fastlog_flushed_tail_address`
- `fastlog_committed_tail_address`

### 28.2 Capacity gauges

- resident pages by state,
- dirty bytes,
- uncommitted bytes,
- append waiters,
- commit waiters,
- read-cache pages,
- open segment descriptors,
- pending GC segments.

### 28.3 Counters

- records and bytes appended,
- pages sealed and flushed,
- flush retries and failures,
- physical commits,
- logical commit requests coalesced,
- checkpoint slot fallbacks during recovery,
- CRC failures,
- short reads and writes,
- segment creations and deletions,
- backpressure events.

### 28.4 Latency histograms

- append latency,
- append backpressure time,
- page flush latency,
- commit latency,
- checkpoint publish latency,
- random read latency,
- scan page-in latency,
- recovery/open latency.

Metrics MUST use partition labels with bounded cardinality.

---

## 29. Testing strategy

### 29.1 Model-based testing

A simple reference model stores records in a vector:

```text
Model state:
  records
  begin index
  committed index

System operations are mirrored into the model.
After every operation or simulated crash, visible records must match.
```

Randomized operation sequences include append, commit, read, scan, truncate,
failure, and reopen.

### 29.2 Fault-injection matrix

Every durability operation is tested with failure:

| Operation | Before | Partial | After but before next step |
|-----------|--------|---------|----------------------------|
| Segment creation | yes | header tear | yes |
| Data write | yes | short write | yes |
| Segment sync | yes | n/a | yes |
| Checkpoint write | yes | torn slot | yes |
| Checkpoint sync | yes | n/a | yes |
| Segment unlink | yes | n/a | yes |
| Directory sync | yes | n/a | yes |

Each case restarts the partition and compares it with the model.

### 29.3 Boundary tests

Required deterministic cases:

- empty payload,
- one-byte payload,
- maximum payload,
- one byte over maximum,
- exact page fit,
- one-byte page overflow and padding,
- frame header at page end,
- exact segment boundary,
- record immediately after segment rotation,
- resident ring wrap,
- 64-bit address overflow rejection,
- checkpoint generation rollover policy,
- begin equal to committed tail,
- commit with no new data but pending truncation.

### 29.4 Recovery tests

- fresh empty open,
- append without commit then reopen,
- small record commit then reopen,
- partial-page commit then reopen and append,
- multiple segments committed then reopen,
- latest checkpoint slot torn,
- older checkpoint slot corrupt but latest valid,
- both slots invalid in non-empty directory,
- missing required segment,
- wrong log UUID,
- changed geometry,
- valid uncommitted records beyond checkpoint tail,
- stale segments below durable begin.

### 29.5 Corruption tests

- record magic corruption,
- header CRC corruption,
- payload CRC corruption,
- payload length corruption,
- frame length corruption,
- logical address corruption,
- unknown record flags,
- segment header corruption,
- checkpoint CRC corruption,
- conflicting equal checkpoint generations.

No corrupt length test may cause out-of-bounds memory access or an oversized I/O.

### 29.6 Concurrency tests

Cooperatively interleave at every yield point:

- append while commit data write is pending,
- two concurrent commit requests,
- truncate during active commit,
- read during segment deletion,
- scan during ring wrap,
- close during page flush,
- close during checkpoint sync,
- read-cache eviction during suspended read.

### 29.7 Sanitizers and tools

The conformance suite SHOULD run under:

- AddressSanitizer,
- UndefinedBehaviorSanitizer,
- ThreadSanitizer where the runtime supports it,
- Valgrind or an equivalent leak checker,
- a coverage-guided decoder fuzzer,
- filesystem fault injection in a Linux VM.

---

## 30. Benchmarks

Correctness gates performance work. Once conformance passes, benchmark:

### 30.1 Append throughput

- payload sizes: 0, 32 B, 256 B, 1 KiB, 16 KiB, maximum,
- one and many partitions,
- CRC enabled as required by V1,
- resident ring sizes from small to production scale.

### 30.2 Commit behavior

- one record per commit,
- periodic commits at 100 us, 1 ms, 10 ms,
- concurrent commit request coalescing,
- cross-segment commits,
- p50, p95, p99, and maximum acknowledgement latency.

### 30.3 Reads

- resident hit,
- read-cache hit,
- disk page-in,
- sequential scan,
- durable tailing scan,
- mixed append/read workload.

### 30.4 Backpressure

Measure writer stall duration with deliberately slow storage and a small
resident ring. The benchmark MUST verify that CPU usage remains low while fibers
are parked.

### 30.5 Recovery

Measure open time versus retained segment count and tail-page size. Normal open
should be proportional to required segment-header validation, not total record
count.

---

## 31. Security and robustness

Persistent files are untrusted input during recovery and read.

Implementations MUST:

- perform checked arithmetic before adding lengths or addresses,
- validate checksums before trusting lengths,
- cap allocations by configured maxima,
- reject path names that do not strictly match the segment grammar,
- avoid following unexpected symlinks in the partition directory,
- ensure a segment header UUID matches the checkpoint UUID,
- never log payload bytes by default,
- expose corruption addresses without exposing record contents,
- create files and directories using configured permissions subject to umask.

Checksums are integrity checks, not authentication. An attacker able to rewrite
the directory can recompute CRC32C. Authenticated storage is outside V1.

---

## 32. Format evolution

### 32.1 Major versions

An unknown major version is incompatible. Open returns `unsupported_version`.

### 32.2 Minor versions

A reader may accept a newer minor version only when:

- the known prefix length is sufficient,
- all unknown fields are optional,
- no unknown must-understand feature flag is set.

### 32.3 Reserved bytes

Writers set reserved bytes to zero. Readers ignore zero reserved bytes and reject
non-zero bytes unless a recognized feature flag defines them.

### 32.4 Migration

Migration is an offline copy:

```text
open old format read-only -> scan committed records -> append to new log -> commit
```

V1 does not rewrite segment files in place.

---

## 33. Implementation plan

### Phase 1: Format and pure Log Core

- Checked arithmetic and explicit endian codecs.
- Record header encode/decode and fuzz tests.
- Address allocation and padding.
- Page state machine.
- In-memory segment and checkpoint stores.
- Model-based append/read/scan tests.

Exit criterion: no kernel I/O, but every state transition and crash point can be
simulated deterministically.

### Phase 2: Reference POSIX durability

- Segment creation and headers.
- Exact pread/pwrite loops.
- Two-slot checkpoint store.
- Commit and recovery.
- Truncation and retryable GC.
- Process locking.

Exit criterion: full conformance suite passes on Linux with repeated process-kill
tests.

### Phase 3: Bounded memory and fibers

- Fixed resident ring.
- Eager sealed-page flush.
- Append backpressure and wakeups.
- Group commit coordinator.
- Concurrent reads and scan prefetch.
- Explicit close and drain.

Exit criterion: ring wrap and all cooperative interleaving tests pass.

### Phase 4: io_uring backend

- Registered arena and staging buffers.
- Registered segment descriptors.
- Batched fixed writes and aligned reads.
- Linked commit durability chains.
- Completion-safe resource lifetime.

Exit criterion: identical conformance results to the POSIX backend under
sanitizers and fault injection.

### Phase 5: Performance and operations

- Page cache policy tuning.
- Segment preallocation experiments.
- Commit batching policy.
- Metrics and dashboards.
- Offline verify and inspect tools.
- Production workload benchmarks.

---

## 34. Rejected alternatives

### 34.1 Recover tail by scanning segment files

Rejected as the primary recovery protocol because:

- preallocation makes file length meaningless,
- valid uncommitted records may exist after a crash,
- scanning cost grows with retained data,
- a valid CRC does not mean an application commit occurred.

Scanning remains useful for offline verification.

### 34.2 Use file length as committed tail

Rejected because file length can include preallocation, alignment padding,
uncommitted writes, and torn extension metadata.

### 34.3 Put commit markers only inside the data stream

This can reduce the number of files, but locating the newest valid marker either
requires scanning or an index that becomes another checkpoint. A dedicated
two-slot checkpoint gives constant-size recovery logic and clear truncation
metadata.

### 34.4 Persist native C++ structs

Rejected because packing, bitfields, alignment, padding, and endianness are ABI
details rather than file-format guarantees.

### 34.5 Seal the whole logical page on every commit

Rejected because frequent small commits would waste most of a large page. V1
uses aligned staging memory to snapshot the partial mutable page while preserving
the exact tail.

### 34.6 Expose uncommitted records by default

Rejected because it encourages applications to treat process-local visibility
as durability. Local visibility remains an explicit option.

### 34.7 Keep delete and copy operations in the frame header

Rejected because FastLog stores opaque records. Application-level event types
belong in the payload schema.

---

## 35. Open questions

The following choices can be resolved through prototypes without changing the
core protocol:

1. Default page size: 64 KiB, 256 KiB, or 1 MiB.
2. Whether the production read path uses direct page-in exclusively.
3. Whether segment headers should include a stronger digest in addition to CRC32C.
4. Whether recovery validates every retained segment header eagerly or lazily.
5. Default segment preallocation policy.
6. Page-cache replacement algorithm.
7. Maximum number of dirty segment descriptors per commit.
8. Whether durable consumer positions belong in a future checkpoint extension.

These are intentionally not correctness blockers for V1.

---

## 36. Acceptance criteria

FastLog V1 is complete when:

1. The persistent codec is fuzzed and contains no unchecked length arithmetic.
2. All storage backends pass the same conformance suite.
3. Every commit crash point recovers either the previous or new complete prefix.
4. A successful small-record commit survives reopen.
5. Appends continue across repeated resident-ring wraps without manual cursor changes.
6. Short writes never advance flushed or committed markers.
7. Concurrent commit requests are serialized and correctly coalesced.
8. Truncation never deletes data required by the latest recoverable checkpoint.
9. Reopening with mismatched geometry fails safely.
10. Corrupt lengths cannot cause out-of-bounds access or oversized I/O.
11. Close drains all kernel references before registered memory is released.
12. The implementation passes sanitizer, fault-injection, and process-kill tests.

---

## Appendix A: Worked example - small commit

Configuration:

```text
page size       = 64 KiB
I/O alignment   = 4 KiB
record payload  = 100 bytes
record frame    = align_up(32 + 100, 8) = 136 bytes
```

After append:

```text
page 0

+---------------------+---------------------------------------------+
| 136-byte frame      | free                                        |
+---------------------+---------------------------------------------+
^                     ^
0                     tail = 136

committed_tail = 0
flushed_tail   = 0
```

Commit snapshots `T = 136` and writes one aligned staging block:

```text
4 KiB staging block

+---------------------+---------------------------------------------+
| 136 committed bytes | zero through byte 4095                      |
+---------------------+---------------------------------------------+
^                     ^                                             ^
0                     T=136                                         4096
```

The checkpoint records `committed_tail = 136`, not 4096. Recovery reads the
block, retains bytes `[0,136)`, zeroes the rest, and resumes append at 136.

---

## Appendix B: Worked example - page boundary

Assume 100 bytes remain in the page and the next user frame requires 200 bytes.

```text
before append

+-----------------------------------------------+-------------------+
| existing records                              | 100 bytes free    |
+-----------------------------------------------+-------------------+

after padding and append

page N
+-----------------------------------------------+-------------------+
| existing records                              | padding frame     |
+-----------------------------------------------+-------------------+

page N+1
+------------------------------+------------------------------------+
| new 200-byte frame           | free                               |
+------------------------------+------------------------------------+
^
returned address
```

The padding frame length is 100. A scanner advances directly to page N+1.

---

## Appendix C: Worked example - ring wrap

Four resident frames currently hold logical pages 8 through 11:

```text
+------------+------------+------------+------------+
| frame 0    | frame 1    | frame 2    | frame 3    |
| page 8     | page 9     | page 10    | page 11    |
| Flushed    | Flushed    | Sealed     | Mutable    |
+------------+------------+------------+------------+
```

When page 11 seals, page 12 needs frame 0:

```text
frame_index(12) = 12 % 4 = 0
```

Page 8 is flushed and unreferenced, so frame 0 can be retagged:

```text
+------------+------------+------------+------------+
| frame 0    | frame 1    | frame 2    | frame 3    |
| page 12    | page 9     | page 10    | page 11    |
| Mutable    | Flushed    | Flushing   | Sealed     |
+------------+------------+------------+------------+
```

A later read of page 8 sees that frame 0's tag is 12 and reads page 8 from disk.

---

## Appendix D: Worked example - crash between data and checkpoint

Initial durable checkpoint:

```text
generation = 40
begin      = 0
tail       = 10,000
```

The application appends through address 15,000 and calls commit.

```text
1. data through 15,000 written       success
2. segment fdatasync                 success
3. checkpoint generation 41 write   not started
4. power failure
```

After restart, segment bytes through 15,000 may be intact, but checkpoint 40 is
the latest valid generation. Recovery sets tail to 10,000 and ignores the last
5,000 bytes.

```text
0                         10,000                      15,000
|                            |                           |
v                            v                           v
+----------------------------+---------------------------+
| recovered committed prefix | ignored stable bytes      |
+----------------------------+---------------------------+
```

This is intentional prefix recovery.

---

## Appendix E: Worked example - truncation

Segments contain 1 GiB of logical data each:

```text
segment 0        segment 1        segment 2        segment 3
[0,1GiB)         [1,2GiB)         [2,3GiB)         [3,4GiB)
```

The application truncates until `2.5 GiB`:

```text
segment 0        segment 1        segment 2        segment 3
fully dead       fully dead       partly dead      retained
delete eligible  delete eligible  retained         retained
```

Before the truncation commit, no segment is deleted. After the checkpoint with
`durable_begin = 2.5 GiB` is synchronized, segments 0 and 1 are deleted. Segment
2 remains because its upper half still contains visible records.

---

## Appendix F: End-to-end lifecycle

```text
                                  FastLog partition lifetime

 +---------+       +-------+       +----------+       +---------+
 | Create  | ----> | Open  | ----> | Closing  | ----> | Closed  |
 +---------+       +---+---+       +----------+       +---------+
                       |
                       |
       +---------------+----------------------------------+
       |               |                |                 |
       v               v                v                 v
   +--------+       +--------+       +--------+       +----------+
   | Append |       | Read   |       | Commit |       | Truncate |
   +---+----+       +--------+       +---+----+       +----+-----+
       |                                 |                 |
       v                                 v                 |
   +--------+       +--------+       +----------+          |
   | Seal   | ----> | Flush  | ----> | Checkpoint| <-------+
   +--------+       +---+----+       +----+-----+
                        |                  |
                        v                  v
                   reuse frames       garbage collect
```

The implementation is correct only when every arrow preserves the invariants in
Section 8.
