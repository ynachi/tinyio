#pragma once

#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <liburing.h>

#include "tinyio/check.hpp"
#include "tinyio/detail/fiber.hpp"
#include "tinyio/detail/ring_buffer.hpp"
#include "tinyio/error.hpp"

namespace tinyio {

class IO;

/// @brief Geometry of the runtime's fixed buffer pool (IoOptions::buffer_pool).
struct BufferPoolConfig {
    size_t slot_size;  ///< Slot size: a power of two >= 4096 that divides 1 GiB.
    uint32_t slots;    ///< Number of slots. Must be > 0.
};

class FixedBufferPool;

// ============================================================================
// FixedBuffer: an exclusive lease on one registered buffer slot
// ============================================================================
//
// Movable, non-copyable and RAII: the slot goes back to its pool when the
// lease is destroyed or explicitly released. Usable like a plain buffer
// (Data()/Ptr()/Size()/First()/Subspan(), span conversions), so ordinary span
// code works on it.
class FixedBuffer {
public:
    FixedBuffer() noexcept = default;

    FixedBuffer(const FixedBuffer&) = delete;
    FixedBuffer& operator=(const FixedBuffer&) = delete;

    FixedBuffer(FixedBuffer&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)),
          data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          ordinal_(std::exchange(other.ordinal_, 0)) {}

    FixedBuffer& operator=(FixedBuffer&& other) noexcept {
        if (this != &other) {
            Release();
            pool_ = std::exchange(other.pool_, nullptr);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            ordinal_ = std::exchange(other.ordinal_, 0);
        }
        return *this;
    }

    ~FixedBuffer() noexcept { Release(); }

    /// @brief Return the slot to its pool. Idempotent.
    void Release() noexcept;

    /// @brief True while the lease holds a slot.
    [[nodiscard]] bool Valid() const noexcept { return pool_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return pool_ != nullptr; }

    /// @brief View the leased memory as a mutable byte span.
    [[nodiscard]] std::span<std::byte> Data() noexcept { return {data_, size_}; }

    /// @brief View the leased memory as a const byte span.
    [[nodiscard]] std::span<const std::byte> Data() const noexcept { return {data_, size_}; }

    [[nodiscard]] std::byte* Ptr() noexcept { return data_; }
    [[nodiscard]] const std::byte* Ptr() const noexcept { return data_; }
    [[nodiscard]] size_t Size() const noexcept { return size_; }

    /// @brief The first n bytes of the slot. Same contract as std::span::first.
    [[nodiscard]] std::span<std::byte> First(const size_t n) noexcept {
        CHECK(n <= size_) << "FixedBuffer::first: n=" << n << " exceeds slot size " << size_;
        return Data().first(n);
    }

    /// @brief The first n bytes of the slot. Same contract as std::span::first.
    [[nodiscard]] std::span<const std::byte> First(const size_t n) const noexcept {
        CHECK(n <= size_) << "FixedBuffer::first: n=" << n << " exceeds slot size " << size_;
        return Data().first(n);
    }

    /// @brief The sub-range [off, off+n) of the slot. Same contract as std::span::subspan.
    [[nodiscard]] std::span<std::byte> Subspan(const size_t off, const size_t n) noexcept {
        CHECK(off <= size_ && n <= size_ - off) << "FixedBuffer::subspan: [" << off << ", " << off
                                                << "+" << n << ") outside slot of " << size_;
        return Data().subspan(off, n);
    }

    /// @brief The sub-range [off, off+n) of the slot. Same contract as std::span::subspan.
    [[nodiscard]] std::span<const std::byte> Subspan(const size_t off,
                                                     const size_t n) const noexcept {
        CHECK(off <= size_ && n <= size_ - off) << "FixedBuffer::subspan: [" << off << ", " << off
                                                << "+" << n << ") outside slot of " << size_;
        return Data().subspan(off, n);
    }

    /// @brief Explicit span conversions for span-taking APIs.
    [[nodiscard]] explicit operator std::span<std::byte>() noexcept { return Data(); }
    [[nodiscard]] explicit operator std::span<const std::byte>() const noexcept { return Data(); }

    /// @brief The slot's ordinal within its pool, in [0, Capacity()).
    [[nodiscard]] uint32_t Ordinal() const noexcept { return ordinal_; }

    /// @brief The pool that owns this slot.
    [[nodiscard]] FixedBufferPool* Pool() const noexcept { return pool_; }

    /// @brief The io_uring registered-buffer index (the SQE buf_index) of the slot.
    [[nodiscard]] uint16_t Index() const noexcept;

private:
    friend class FixedBufferPool;

    FixedBuffer(FixedBufferPool* pool, std::byte* data, size_t size, uint32_t ordinal) noexcept
        : pool_(pool), data_(data), size_(size), ordinal_(ordinal) {}

    FixedBufferPool* pool_ = nullptr;
    std::byte* data_ = nullptr;
    size_t size_ = 0;
    uint32_t ordinal_ = 0;
};

// ============================================================================
// FixedBufferPool: a registered buffer pool
// ============================================================================
//
// A pool of fixed-size slots carved out of one huge-page backed mapping and
// registered with io_uring, so a read or write can name the slot instead of
// a raw address (READ_FIXED / WRITE_FIXED) and the kernel does not have to
// copy through a bounce buffer.
//
// Memory layout and registration slicing:
//
//   base_
//     | 0                   1 GiB                 2 GiB               2.5 GiB
//     +---------------------+---------------------+---------------------+--------------+
//     |      Chunk 0        |      Chunk 1        |      Chunk 2        |   Chunk 3    |
//     +---------------------+---------------------+---------------------+--------------+
//     ^                     ^                     ^                     ^
//     | iovec[0]            | iovec[1]            | iovec[2]           | iovec[3]
//
// Invariants:
//
// 1. One contiguous mapping, 2 MiB-aligned base AND length (THP): the
//    mapping is placed so base_ itself sits on a 2 MiB boundary (one guard
//    2 MiB is mapped, the base rounded up inside it, head/tail slivers
//    returned), and the length is rounded up to 2 MiB. MADV_HUGEPAGE then
//    lets THP collapse the region into 2 MiB compound pages -- fewer
//    page-table walks and TLB misses during DMA.
//
// 2. Power-of-two divisibility: slot_size must be a power of two >= 4096
//    that divides 1 GiB. io_uring caps each registered iovec at 1 GiB, so
//    larger pools are registered as consecutive 1 GiB iovecs; because 1 GiB
//    is a multiple of slot_size, every chunk boundary falls exactly on a
//    slot boundary -- no slot ever straddles two registered entries.
//
// 3. Branchless registered index resolution:
//    buf_index = first_index_ + ((ptr - base_) >> 30) -- one shift, no
//    interval trees, no per-slot metadata.
//
// 4. Direct handoff to waiters: when a slot is released while fibers wait
//    for one, the slot is handed to the oldest waiter instead of going back
//    on the free list, so a new arrival can never steal it. Waiters are
//    served strictly FIFO.
//
// 5. Clean, idempotent shutdown: Close() wakes every still-parked fiber;
//    each resumes with operation_canceled. A fiber a slot was already
//    handed to still gets it, and leases already handed out stay valid to
//    destroy. The runtime never closes its own pool on shutdown: a fiber
//    still parked once it has quiesced is a leaked lease, and it aborts.
class FixedBufferPool {
public:
    static constexpr size_t kOneGiB = 1ULL << 30U;               // 1,073,741,824 bytes
    static constexpr size_t kHugePageSize = 2ULL * 1024 * 1024;  // 2 MiB THP alignment
    static constexpr size_t kPageSize = 4096;                    // 4 KiB page size

    /// @brief Map a pool of @p slots slots of @p slot_size bytes.
    /// @param slot_size Size of each slot. Must be a power of two >= 4096 that
    ///        divides 1 GiB.
    /// @param slots Number of slots. Must be > 0.
    /// @throws std::invalid_argument on a bad geometry, std::system_error when
    ///         the mapping cannot be created.
    FixedBufferPool(size_t slot_size, uint32_t slots);

    ~FixedBufferPool() noexcept;

    FixedBufferPool(const FixedBufferPool&) = delete;
    FixedBufferPool& operator=(const FixedBufferPool&) = delete;
    // Pinned, registered memory makes the pool address-fixed: moving it is
    // meaningless. Construct it where it will live.
    FixedBufferPool(FixedBufferPool&&) = delete;
    FixedBufferPool& operator=(FixedBufferPool&&) = delete;

    /// @brief Take a slot without blocking.
    /// @return The slot, or EAGAIN when the pool is exhausted.
    [[nodiscard]] Result<FixedBuffer> Take() noexcept;

    /// @brief Take a slot, parking the running fiber while the pool is empty.
    ///
    /// The fiber is woken when a slot is handed to it, and resumes with
    /// operation_canceled if the pool is closed before a slot was handed to it.
    ///
    /// @note Owner-thread only.
    [[nodiscard]] Result<FixedBuffer> Acquire(IO& io) noexcept;

    /// @brief Permanently shut the pool down: every parked fiber wakes with
    ///        operation_canceled. Idempotent; leases stay releasable.
    void Close() noexcept;

    /// @brief Give up on the parked fibers without waking them.
    ///
    /// The runtime calls this before it destroys its fibers: a fiber that is
    /// parked on a slot points at memory the runtime is about to free, so the
    /// pool must not unpark it afterwards.
    void Detach() noexcept {
        owner_ = nullptr;
        detail::fiber::Fiber* fiber = nullptr;
        while (waiters_.Pop(fiber)) {
        }
        handed_.Clear();
    }

    /// @brief Append this pool's <= 1 GiB chunks to a registration table.
    /// @return The starting table index assigned to this pool.
    uint16_t AppendRegions(std::vector<iovec>& table);

    /// @brief The registered-buffer index of a pointer inside this pool.
    [[nodiscard]] uint16_t RegistrationIndex(const std::byte* ptr) const noexcept {
        CHECK(ptr >= base_ && ptr < base_ + total_bytes_)
            << "RegistrationIndex on a pointer outside the pool mapping";
        const auto offset = static_cast<size_t>(ptr - base_);
        const size_t index = static_cast<size_t>(first_index_) + (offset >> 30U);
        CHECK(index <= 65535) << "registered buffer index overflows the SQE buf_index field";
        return static_cast<uint16_t>(index);
    }

    /// @brief The runtime that unparks the fibers waiting on this pool.
    void SetOwner(IO* io) noexcept { owner_ = io; }
    [[nodiscard]] IO* Owner() const noexcept { return owner_; }

    [[nodiscard]] size_t SlotSize() const noexcept { return slot_size_; }
    [[nodiscard]] uint32_t Capacity() const noexcept { return slots_; }

    /// @brief Slots on the free list. Slots handed to a waiter are not counted.
    [[nodiscard]] uint32_t Available() const noexcept {
        return static_cast<uint32_t>(free_slots_.size());
    }

    /// @brief Fibers currently parked on Acquire().
    [[nodiscard]] size_t Waiting() const noexcept { return waiters_.Size(); }

    [[nodiscard]] bool Closed() const noexcept { return closed_; }
    [[nodiscard]] std::byte* Base() const noexcept { return base_; }
    [[nodiscard]] size_t TotalBytes() const noexcept { return total_bytes_; }
    [[nodiscard]] size_t AllocatedBytes() const noexcept { return allocated_bytes_; }

private:
    friend class FixedBuffer;

    /// @brief Return a slot: hand it to the oldest waiter, or push it back on
    ///        the free list.
    void Release(uint32_t ordinal) noexcept;

    [[nodiscard]] FixedBuffer MakeLease(uint32_t ordinal) noexcept {
        return FixedBuffer{this, base_ + static_cast<size_t>(ordinal) * slot_size_, slot_size_,
                           ordinal};
    }

    std::byte* base_ = nullptr;
    size_t slot_size_ = 0;
    uint32_t slots_ = 0;
    size_t total_bytes_ = 0;
    size_t allocated_bytes_ = 0;
    uint16_t first_index_ = 0;
    bool closed_ = false;

    // Free slots stack (LIFO for CPU cache locality): recently returned slots
    // stay hot in cache.
    std::vector<uint32_t> free_slots_;

    // Parked Acquire() fibers, FIFO.
    detail::RingBuffer<detail::fiber::Fiber*> waiters_;

    // Slots handed to woken waiters, in wake order. A woken fiber picks the
    // oldest one: all slots of a pool are interchangeable, so the assignment
    // does not have to match.
    detail::RingBuffer<uint32_t> handed_;

    // The runtime that unparks the waiters.
    IO* owner_ = nullptr;
};

}  // namespace tinyio
