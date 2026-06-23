#pragma once

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace tinyio::detail {

/// A bounded, lock-free Multi-Producer Single-Consumer (MPSC) queue.
/// Wait-free for the consumer, lock-free for producers.
/// Pre-allocates all memory to eliminate `new`/`delete` overhead on the hot path.
template <typename T>
class MpscQueue {
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
  std::vector<Slot> slots_;

 public:
  /// Capacity MUST be a power of 2 (e.g., 1024, 4096).
  explicit MpscQueue(const size_t capacity = 4096)
      : capacity_mask_(capacity - 1), slots_(capacity) {
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
  bool Enqueue(T&& data) noexcept {
    size_t head = head_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& slot = slots_[head & capacity_mask_];

      // Is it our turn to claim this slot?
      if (const size_t turn = slot.turn.load(std::memory_order_acquire); turn == head) {
        // Claim the slot via compare-and-swap
        if (head_.compare_exchange_weak(head, head + 1, std::memory_order_relaxed)) {
          slot.data = std::move(data);

          // Signal to the consumer that the data is ready
          slot.turn.store(head + 1, std::memory_order_release);
          return true;
        }
      } else if (turn < head) {
        // The consumer hasn't finished reading this slot yet -> Queue is full!
        return false;
      } else {
        // Another producer beat us to it, reload head and try again
        head = head_.load(std::memory_order_relaxed);
      }
    }
  }

  /// Try to dequeue an item. Must ONLY be called from the single consumer thread.
  bool TryDequeue(T& out_data) noexcept {
    const size_t tail = tail_;
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

    // Queue empty or producer hasn't finished writing
    return false;
  }

  /// Dequeue up to max_count items. Must ONLY be called from the single consumer thread.
  size_t TryDequeueBulk(T* out_items, const size_t max_count) noexcept {
    size_t count = 0;
    while (count < max_count) {
      if (TryDequeue(out_items[count])) {
        count++;
      } else {
        break;
      }
    }
    return count;
  }

  /// Consumer-only. Conservative: returns false if any slot has been
  /// *claimed*, even if its payload isn't published yet. This is exactly
  /// what shutdown loops need — a claimed slot will be published shortly
  /// (the producer is between CAS and the release store).
  bool Empty() const noexcept { return head_.load(std::memory_order_acquire) == tail_; }
};

}  // namespace tinyio::detail
