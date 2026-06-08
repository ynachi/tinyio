#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>
#include <cstdlib>

namespace tinyio::detail {

/// A bounded, lock-free Multi-Producer Single-Consumer (MPSC) queue.
/// Wait-free for the consumer, lock-free for producers.
/// Pre-allocates all memory to eliminate `new`/`delete` overhead on the hot path.
template <typename T>
class MpscQueue {
private:
    struct Slot {
        std::atomic<size_t> turn;
        T data;
    };

    // head_ is modified by all producers — keep on its own cache line.
    alignas(64) std::atomic<size_t> head_{0};
    
    // tail_ is only modified by the single consumer — separate cache line.
    // It doesn't need to be atomic because producers synchronize via `Slot::turn`.
    alignas(64) size_t tail_{0}; 
    
    const size_t capacity_mask_;
    std::unique_ptr<Slot[]> slots_;

public:
    /// Capacity MUST be a power of 2 (e.g., 1024, 4096).
    explicit MpscQueue(size_t capacity = 4096) 
        : capacity_mask_(capacity - 1),
          slots_(new Slot[capacity]) 
    {
        // Enforce power of two requirement for fast modulo bitwise operations
        if ((capacity == 0) || ((capacity & capacity_mask_) != 0)) {
            std::abort();
        }
        
        for (size_t i = 0; i < capacity; ++i) {
            slots_[i].turn.store(i, std::memory_order_relaxed);
        }
    }

    // Disable copy and move
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    /// Enqueue an item. Can be called from any thread.
    /// Returns true on success, false if the queue is full.
    bool enqueue(T&& data) noexcept {
        size_t head = head_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[head & capacity_mask_];
            size_t turn = slot.turn.load(std::memory_order_acquire);
            
            // Is it our turn to claim this slot?
            if (turn == head) {
                // Claim the slot via compare-and-swap
                if (head_.compare_exchange_weak(head, head + 1, std::memory_order_relaxed)) {
                    slot.data = std::move(data);
                    
                    // Signal to the consumer that the data is ready
                    slot.turn.store(head + 1, std::memory_order_release);
                    return true;
                }
            } 
            else if (turn < head) {
                // The consumer hasn't finished reading this slot yet -> Queue is full!
                return false;
            } 
            else {
                // Another producer beat us to it, reload head and try again
                head = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /// Try to dequeue an item. Must ONLY be called from the single consumer thread.
    bool try_dequeue(T& out_data) noexcept {
        size_t tail = tail_;
        Slot& slot = slots_[tail & capacity_mask_];
        
        size_t turn = slot.turn.load(std::memory_order_acquire);
        
        // If turn == tail + 1, the producer has completely finished writing
        if (turn == tail + 1) {
            out_data = std::move(slot.data);
            
            // Tell producers this slot is now free for the next wrap-around cycle
            slot.turn.store(tail + capacity_mask_ + 1, std::memory_order_release);
            tail_ = tail + 1;
            return true;
        }
        
        return false; // Queue empty or producer hasn't finished writing
    }

    /// Dequeue up to max_count items. Must ONLY be called from the single consumer thread.
    size_t try_dequeue_bulk(T* out_items, size_t max_count) noexcept {
        size_t count = 0;
        while (count < max_count) {
            if (try_dequeue(out_items[count])) {
                count++;
            } else {
                break;
            }
        }
        return count;
    }

    /// Check if the queue is empty. Must ONLY be called from the single consumer thread.
    bool empty() const noexcept {
        size_t tail = tail_;
        Slot& slot = slots_[tail & capacity_mask_];
        return slot.turn.load(std::memory_order_relaxed) != tail + 1;
    }
};

} // namespace tinyio::detail
