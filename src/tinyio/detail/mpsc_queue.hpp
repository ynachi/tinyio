#pragma once

#ifdef BLOCK_SIZE
#    undef BLOCK_SIZE
#endif
#include <memory>
#include <utility>

#include <ylt/util/concurrentqueue.h>

namespace tinyio::detail {

/// A bounded, lock-free Multi-Producer Single-Consumer (MPSC) queue.
/// Wait-free for the consumer, lock-free for producers.
/// Wraps moodycamel::ConcurrentQueue, taken from YLT: easylog pulls the same
/// header in, and two copies of it cannot live in one translation unit.
/// YLT keeps moodycamel under ylt::detail, hence the qualified name.
template <typename T>
class MpscQueue {
    ylt::detail::moodycamel::ConcurrentQueue<T> queue_;

public:
    explicit MpscQueue(const size_t capacity = 4096) : queue_(capacity) {}

    // Disable copy and move
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    /// Enqueue an item. Can be called from any thread.
    /// Returns true on success, false if the queue is full.
    bool Enqueue(T&& data) noexcept {
        // try_enqueue does not allocate memory. It will fail if the pre-allocated
        // capacity is exhausted.
        return queue_.try_enqueue(std::move(data));
    }

    /// Try to dequeue an item. Must ONLY be called from the single consumer thread.
    bool TryDequeue(T& out_data) noexcept { return queue_.try_dequeue(out_data); }

    /// Dequeue up to max_count items. Must ONLY be called from the single consumer thread.
    size_t TryDequeueBulk(T* out_items, const size_t max_count) noexcept {
        return queue_.try_dequeue_bulk(out_items, max_count);
    }

    /// Consumer-only. Conservative: returns false if any slot has been
    /// *claimed*, even if its payload isn't published yet.
    bool Empty() const noexcept { return queue_.size_approx() == 0; }
};

}  // namespace tinyio::detail
