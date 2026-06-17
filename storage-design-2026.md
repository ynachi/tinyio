# Blueprint for a 2026 Storage Engine

Most modern databases (LevelDB, RocksDB) were designed in the early 2010s for spinning hard drives and early SSDs. If we are building a state-of-the-art storage engine from scratch in 2026 using `tinyio` (C++ fibers + `io_uring`), it must be designed from the ground up to exploit modern hardware realities: 64+ core CPUs, NVMe SSDs, and advanced kernel bypasses.

Here are the core architectural pillars for the 2026 storage engine:

## 1. Key-Value Separation (The "WiscKey" Architecture)
Traditional LSM-Trees suffer from massive "Write Amplification" during background compaction because they constantly read and rewrite both keys *and* values to sort them.
* **The 2026 Way:** Separate them! The LSM-Tree should only store Keys and a lightweight pointer (file offset). The actual heavy Values are appended to a completely separate, append-only "Value Log" (Blob storage). Compaction on the LSM-Tree becomes lightning fast, saving NVMe lifespan and CPU cycles.

## 2. Thread-per-Core / Shared-Nothing Concurrency
Traditional databases use `std::mutex` to protect shared in-memory structures (like the MemTable). On modern 128-core CPUs, thread contention destroys throughput.
* **The 2026 Way:** Implement a "Shared-Nothing" architecture. Pin one `tinyio` event loop to one CPU core. Shard the keyspace (e.g., Keys A-M to Core 1, N-Z to Core 2). Fibers can traverse B-Trees and MemTables entirely lock-free because they never cross CPU boundaries. 

## 3. Pure User-Space Memory Management (`O_DIRECT`)
Relying on the Linux OS Page Cache (`mmap` or buffered I/O) is terrible for databases because the kernel uses generic LRU algorithms, leading to unpredictable tail-latency spikes and memory stalls.
* **The 2026 Way:** Open all database files using `O_DIRECT` and use `io_uring` to bypass the Linux kernel entirely.
    * **I/O Transport:** Use tiny, page-aligned **Fixed Buffers** (`IORING_REGISTER_BUFFERS`) to stream data to disk via DMA with zero syscalls.
    * **Database Page Cache:** Build a massive custom Buffer Pool in user-space to cache 4KB B-Tree blocks in RAM. This gives us tyrannical control over what stays in memory and what gets evicted.

## 4. Zoned Namespaces (ZNS) Readiness
Modern hyperscalers are moving away from standard SSDs to ZNS NVMe drives. Standard SSDs perform background Garbage Collection (via the FTL), causing random latency spikes. ZNS exposes the raw flash zones directly to the software, but requires the software to be strictly append-only.
* **The 2026 Way:** Because our `BufferedFile` and Value Log are designed to be strictly append-only and block-aligned, the engine is naturally compatible with next-generation ZNS hardware.

## 5. Hardware-Accelerated Probing (SIMD)
When searching SSTables, we must avoid unnecessary disk reads.
* **The 2026 Way:** Instead of standard Bloom filters, use **Ribbon Filters** or **Blocked Bloom Filters** optimized with AVX-512 / AVX2 vector instructions. By using SIMD, the CPU can check if a key exists in the cache in single-digit nanoseconds.

## Summary: The Lowest Level (`BufferedFile`)
The foundation of this entire architecture rests on the `BufferedFile`. It wraps a file descriptor opened with `O_DIRECT`, acquires 32KB fixed buffers from a localized memory pool, buffers small fiber writes lock-free, and dispatches block-aligned chunks asynchronously to `io_uring` without ever blocking the thread.
