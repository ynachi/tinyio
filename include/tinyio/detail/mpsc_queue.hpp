#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <utility>

namespace tinyio::detail {

/// A lock-free, unbounded, multi-producer single-consumer (MPSC) queue.
/// Wait-free for producers, lock-free for the consumer.
/// Based on the intrusive MPSC queue design (Vyukov).
template <typename T>
class MpscQueue {
    struct Node {
        T data;
        std::atomic<Node*> next{nullptr};

        Node() = default;
        explicit Node(T&& d) : data(std::move(d)) {}
    };

    // head_ is modified by all producers — keep on its own cache line.
    alignas(64) std::atomic<Node*> head_;
    // tail_ is only modified by the single consumer — separate cache line.
    alignas(64) Node* tail_;

public:
    MpscQueue() {
        Node* stub = new Node();
        head_.store(stub, std::memory_order_relaxed);
        tail_ = stub;
    }

    ~MpscQueue() {
        T dummy;
        while (try_dequeue(dummy)) {}
        delete tail_;
    }

    // Disable copy and move
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    /// Enqueue an item. Can be called from any thread.
    /// Returns true on success, false on allocation failure.
    bool enqueue(T&& data) noexcept {
        Node* node = new (std::nothrow) Node(std::move(data));
        if (node == nullptr) [[unlikely]]
        {
            return false;
        }
        Node* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
        return true;
    }

    /// Try to dequeue an item. Must only be called from the single consumer thread.
    bool try_dequeue(T& out_data) noexcept {
        Node* tail = tail_;
        Node* next = tail->next.load(std::memory_order_acquire);

        if (next) {
            out_data = std::move(next->data);
            tail_ = next;
            delete tail; // delete the old stub
            return true;
        }

        return false;
    }

    /// Dequeue up to max_count items. Must only be called from the single consumer thread.
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

    /// Check if the queue is empty. Must only be called from the single consumer thread.
    bool empty() const noexcept {
        // Consumer owns tail_; relaxed is sufficient — producers synchronise
        // through the acq_rel exchange in enqueue, not through this load.
        return head_.load(std::memory_order_relaxed) == tail_;
    }
};

} // namespace tinyio::detail
