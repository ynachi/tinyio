#include "tinyio/runtime/buffer_pool.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include "tinyio/runtime/runtime.hpp"

namespace tinyio {

void FixedBuffer::Release() noexcept {
    if (pool_ != nullptr) {
        auto* pool = std::exchange(pool_, nullptr);
        data_ = nullptr;
        size_ = 0;
        pool->Release(ordinal_);
    }
}

uint16_t FixedBuffer::Index() const noexcept {
    CHECK(pool_ != nullptr) << "FixedBuffer::index on an empty lease";
    return pool_->RegistrationIndex(data_);
}

FixedBufferPool::FixedBufferPool(const size_t slot_size, const uint32_t slots)
    : slot_size_(slot_size), slots_(slots) {
    // Invariant 2: slot_size must be a power of two >= 4096 dividing 1 GiB.
    if (slot_size < kPageSize || (slot_size & (slot_size - 1)) != 0 || (kOneGiB % slot_size) != 0) {
        throw std::invalid_argument(
            "slot_size must be a power of two >= 4096 that evenly divides 1 GiB");
    }

    if (slots == 0) {
        throw std::invalid_argument("slots must be > 0");
    }

    // No overflow check: slot_size is validated <= 1 GiB above and slots fits
    // uint32_t, so the product is bounded below 2^62.
    total_bytes_ = slot_size_ * static_cast<size_t>(slots_);

    // Invariant 1: align BOTH length and base to 2 MiB for THP. Map one extra
    // 2 MiB, carve an aligned base out of the mapping, and return the head and
    // tail slivers so the final mapping is exactly [base_, base_ +
    // allocated_bytes_) on a 2 MiB boundary.
    allocated_bytes_ = (total_bytes_ + kHugePageSize - 1) & ~(kHugePageSize - 1);

    // Pre-reserve the free list so Release() stays noexcept in steady state.
    free_slots_.reserve(slots_);
    for (uint32_t i = slots_; i != 0; --i) {
        free_slots_.push_back(i - 1);
    }

    const size_t map_bytes = allocated_bytes_ + kHugePageSize;
    void* mapping =
        ::mmap(nullptr, map_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        throw std::system_error(errno, std::system_category(), "mmap buffer pool failed");
    }

    const auto raw = reinterpret_cast<uintptr_t>(mapping);
    const auto aligned = (raw + kHugePageSize - 1) & ~(kHugePageSize - 1);
    base_ = reinterpret_cast<std::byte*>(aligned);
    (void)::munmap(mapping, aligned - raw);
    (void)::munmap(base_ + allocated_bytes_, (raw + map_bytes) - (aligned + allocated_bytes_));

    (void)::madvise(base_, allocated_bytes_, MADV_HUGEPAGE);
}

FixedBufferPool::~FixedBufferPool() noexcept {
    Close();

    if (base_ != nullptr) {
        (void)::munmap(base_, allocated_bytes_);
        base_ = nullptr;
    }
}

Result<FixedBuffer> FixedBufferPool::Take() noexcept {
    if (closed_) {
        return Error::FailErrc(std::errc::operation_canceled, "pool closed");
    }
    if (free_slots_.empty()) {
        return Error::FailErrno(EAGAIN, "fixed_buffer_pool.take");
    }

    // LIFO pop: the most recently used slot is the hot one in cache.
    const uint32_t ordinal = free_slots_.back();
    free_slots_.pop_back();
    return MakeLease(ordinal);
}

Result<FixedBuffer> FixedBufferPool::Acquire(IO& io) noexcept {
    CHECK(io.IsOwnerThread()) << "FixedBufferPool::acquire called from a foreign thread";

    if (owner_ == nullptr) {
        owner_ = &io;
    }
    CHECK(owner_ == &io) << "FixedBufferPool::acquire called with a foreign runtime";

    if (auto buffer = Take(); buffer.has_value()) {
        return buffer;
    }
    if (closed_) {
        return Error::FailErrc(std::errc::operation_canceled, "pool closed");
    }

    auto* fiber = io.RunningFiber();
    CHECK(fiber != nullptr) << "FixedBufferPool::acquire called outside a fiber";

    // Park until a slot is handed to us, or the pool is closed.
    waiters_.Push(fiber);
    io.ParkRunning(detail::fiber::WaitKind::buffer_pool);

    // Resumed. A slot handed over is ours even if the pool closed before we
    // ran: checking closed_ first would strand it in handed_ for good.
    uint32_t ordinal = 0;
    if (handed_.Pop(ordinal)) {
        return MakeLease(ordinal);
    }
    // Woken without a slot: only Close() does that.
    return Error::FailErrc(std::errc::operation_canceled, "pool closed");
}

void FixedBufferPool::Close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;

    // Wake every parked fiber: each one resumes with operation_canceled.
    while (!waiters_.Empty()) {
        detail::fiber::Fiber* fiber = nullptr;
        if (!waiters_.Pop(fiber)) {
            break;
        }
        if (owner_ != nullptr) {
            owner_->Unpark(fiber);
        }
    }
}

uint16_t FixedBufferPool::AppendRegions(std::vector<iovec>& table) {
    first_index_ = static_cast<uint16_t>(table.size());

    // io_uring caps a registered iovec at 1 GiB, so the mapping is registered
    // as consecutive 1 GiB chunks (the last one shorter).
    for (size_t offset = 0; offset < total_bytes_; offset += kOneGiB) {
        const size_t length = std::min(kOneGiB, total_bytes_ - offset);
        CHECK(table.size() < 65535) << "registered buffer index space exhausted";
        table.push_back(iovec{.iov_base = base_ + offset, .iov_len = length});
    }
    return first_index_;
}

void FixedBufferPool::Release(const uint32_t ordinal) noexcept {
    if (closed_) {
        // Nobody can be waiting for a slot anymore; the pool is going away.
        free_slots_.push_back(ordinal);
        return;
    }

    // Direct handoff (Invariant 4): the slot goes to the oldest waiter instead
    // of back on the free list, so a new arrival cannot steal it.
    detail::fiber::Fiber* fiber = nullptr;
    if (!waiters_.Empty() && waiters_.Pop(fiber)) {
        handed_.Push(ordinal);
        if (owner_ != nullptr) {
            owner_->Unpark(fiber);
        }
        return;
    }

    free_slots_.push_back(ordinal);
}

}  // namespace tinyio
