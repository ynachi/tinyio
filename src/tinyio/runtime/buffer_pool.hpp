#pragma once

#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <liburing.h>

#include "tinyio/check.hpp"
#include "tinyio/detail/fiber.hpp"
#include "tinyio/error.hpp"

namespace tinyio {

class IO;

/// @brief Geometry of one fixed buffer pool (IoOptions::buffer_pools).
struct BufferPoolConfig {
    size_t slot_size;  ///< Slot size: a power of two >= 4096 that divides 1 GiB.
    uint32_t slots;    ///< Number of slots. Must be > 0.
};

class FixedBufferPool;

// ============================================================================
// FixedRegion: a bounds-checked sub-slice of a registered FixedBuffer
// ============================================================================
// Eliminates the error-prone 5-argument WriteFixed(fd, buf, len, file_off, buf_off)
// positional integer mix-up by binding [buffer_offset, buffer_offset + len)
// directly to the buffer via buf.Slice(offset, len).
struct FixedRegion {
    const FixedBufferPool* pool = nullptr;
    std::byte* data = nullptr;
    size_t size = 0;
    uint16_t buf_index = 0;

    [[nodiscard]] std::byte* Ptr() noexcept { return data; }
    [[nodiscard]] const std::byte* Ptr() const noexcept { return data; }
    [[nodiscard]] size_t Size() const noexcept { return size; }
    [[nodiscard]] uint16_t Index() const noexcept { return buf_index; }
    [[nodiscard]] const FixedBufferPool* Pool() const noexcept { return pool; }
    [[nodiscard]] std::span<std::byte> Data() noexcept { return {data, size}; }
    [[nodiscard]] std::span<const std::byte> Data() const noexcept { return {data, size}; }
};

// ============================================================================
// FixedBuffer: an exclusive lease on one registered buffer slot
// ============================================================================
// Movable, non-copyable and RAII: the slot goes back to its pool when the
// lease is destroyed or explicitly released. Usable like a plain buffer
// (Data()/Ptr()/Size()/First()/Subspan()/Slice(), span conversions), so ordinary
// span code works on it.
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

    /// @brief Create a registered sub-slice [off, off+n) for ReadFixed / WriteFixed.
    /// @code
    ///   TRY(n, io.WriteFixed(fd, buf.Slice(4096, 8192), file_offset, RWF_DSYNC));
    /// @endcode
    [[nodiscard]] FixedRegion Slice(const size_t off, const size_t n) const noexcept {
        CHECK(pool_ != nullptr) << "FixedBuffer::slice on an empty lease";
        CHECK(off <= size_ && n > 0 && n <= size_ - off)
            << "FixedBuffer::slice: [" << off << ", " << off << "+" << n << ") outside slot of "
            << size_;
        return FixedRegion{
            .pool = pool_,
            .data = data_ + off,
            .size = n,
            .buf_index = Index(),
        };
    }

    /// @brief Explicit span conversions for span-taking APIs.
    [[nodiscard]] explicit operator std::span<std::byte>() noexcept { return Data(); }
    [[nodiscard]] explicit operator std::span<const std::byte>() const noexcept { return Data(); }

    /// @brief Conversion to FixedRegion covering the full slot.
    ///
    /// Explicit on purpose: an implicit conversion would make the two
    /// WriteFixed overloads ambiguous for a call that fits both, because
    /// `int -> rw_flags` is an exact match while `int -> off_t` is a
    /// conversion, and overload resolution needs one candidate to be no worse
    /// in *every* argument.
    ///
    /// @code
    ///   FixedRegion whole = buf;          // direct initialization
    /// @endcode
    [[nodiscard]] explicit operator FixedRegion() const noexcept { return Slice(0, size_); }

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
class FixedBufferPool {
public:
    static constexpr size_t kOneGiB = 1ULL << 30U;               // 1,073,741,824 bytes
    static constexpr size_t kHugePageSize = 2ULL * 1024 * 1024;  // 2 MiB THP alignment
    static constexpr size_t kPageSize = 4096;                    // 4 KiB page size

    FixedBufferPool(size_t slot_size, uint32_t slots);
    ~FixedBufferPool() noexcept;

    FixedBufferPool(const FixedBufferPool&) = delete;
    FixedBufferPool& operator=(const FixedBufferPool&) = delete;
    FixedBufferPool(FixedBufferPool&&) = delete;
    FixedBufferPool& operator=(FixedBufferPool&&) = delete;

    [[nodiscard]] Result<FixedBuffer> Take() noexcept;
    [[nodiscard]] Result<FixedBuffer> Acquire(IO& io) noexcept;
    void Close() noexcept;

    void Detach() noexcept {
        owner_ = nullptr;
        waiters_.Clear();
    }

    uint16_t AppendRegions(std::vector<iovec>& table);

    [[nodiscard]] uint16_t RegistrationIndex(const std::byte* ptr) const noexcept {
        CHECK(ptr >= base_ && ptr < base_ + total_bytes_)
            << "RegistrationIndex on a pointer outside the pool mapping";
        const auto offset = static_cast<size_t>(ptr - base_);
        const size_t index = static_cast<size_t>(first_index_) + (offset >> 30U);
        CHECK(index <= 65535) << "registered buffer index overflows the SQE buf_index field";
        return static_cast<uint16_t>(index);
    }

    void SetOwner(IO* io) noexcept { owner_ = io; }
    [[nodiscard]] IO* Owner() const noexcept { return owner_; }
    [[nodiscard]] size_t SlotSize() const noexcept { return slot_size_; }
    [[nodiscard]] uint32_t Capacity() const noexcept { return slots_; }
    [[nodiscard]] uint32_t Available() const noexcept {
        return static_cast<uint32_t>(free_slots_.size());
    }
    [[nodiscard]] size_t Waiting() const noexcept { return waiters_.Size(); }
    [[nodiscard]] bool Closed() const noexcept { return closed_; }
    [[nodiscard]] std::byte* Base() const noexcept { return base_; }
    [[nodiscard]] size_t TotalBytes() const noexcept { return total_bytes_; }
    [[nodiscard]] size_t AllocatedBytes() const noexcept { return allocated_bytes_; }

private:
    friend class FixedBuffer;

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
    std::vector<uint32_t> free_slots_;
    FiberWaitQueue waiters_;
    IO* owner_ = nullptr;
};

}  // namespace tinyio