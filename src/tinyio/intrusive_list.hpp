#pragma once

#include <cstddef>

namespace tinyio {

/// \brief A generic hook for intrusive linked lists.
/// \tparam Tag A unique tag type to allow a single struct to be in multiple lists.
template <typename Tag>
struct IntrusiveHook {
  // Initialize to point to itself (unlinked/circular state)
  IntrusiveHook* prev = this;
  IntrusiveHook* next = this;

  [[nodiscard]] bool IsLinked() const noexcept { return next != this; }
};

namespace detail {
/// Mixin that holds the size counter only when TrackSize is true.
template <bool TrackSize>
struct SizeTracker {
  std::size_t size_ = 0;
  void Inc() noexcept { ++size_; }
  void Dec() noexcept { --size_; }
  [[nodiscard]] std::size_t Size() const noexcept { return size_; }
};

template <>
struct SizeTracker<false> {
  static void Inc() noexcept {}
  static void Dec() noexcept {}
};
}  // namespace detail

/// \brief A branchless, circular, doubly-linked intrusive list using dummy heads.
/// \tparam T The node type (must publicly inherit from IntrusiveHook<Tag>).
/// \tparam Tag The unique tag identifying which hook to use.
template <typename T, typename Tag, bool TrackSize = true>
class IntrusiveList : detail::SizeTracker<TrackSize> {
  using Tracker = detail::SizeTracker<TrackSize>;

 public:
  constexpr IntrusiveList() noexcept = default;

  [[nodiscard]] bool Empty() const noexcept { return dummy_.next == &dummy_; }

  [[nodiscard]] std::size_t Size() const noexcept
    requires TrackSize
  {
    return Tracker::Size();
  }

  /// \brief Pushes a node to the front of the list (O(1), branchless)
  void PushFront(T* node) noexcept { InsertBetween(GetHook(node), &dummy_, dummy_.next); }

  /// \brief Pushes a node to the back of the list (O(1), branchless)
  /// This gives the list perfect FIFO Queue semantics.
  void PushBack(T* node) noexcept { InsertBetween(GetHook(node), dummy_.prev, &dummy_); }

  /// \brief Pops a node from the front of the list (O(1), branchless)
  [[nodiscard]] T* PopFront() noexcept {
    if (Empty()) {
      return nullptr;
    }
    IntrusiveHook<Tag>* hook = dummy_.next;
    RemoveHook(hook);
    return GetNode(hook);
  }

  /// \brief Removes a specific node from the list (O(1), branchless)
  void Remove(T* node) noexcept {
    IntrusiveHook<Tag>* hook = GetHook(node);
    if (hook->IsLinked()) {
      RemoveHook(hook);
    }
  }

  template <class Fn>
  void ForEach(Fn&& fn) noexcept {
    auto* hook = dummy_.next;
    while (hook != &dummy_) {
      auto* next = hook->next;
      fn(*GetNode(hook));
      hook = next;
    }
  }

 private:
  IntrusiveHook<Tag> dummy_;

  static IntrusiveHook<Tag>* GetHook(T* node) noexcept {
    // Safe upcast from derived to base hook
    return static_cast<IntrusiveHook<Tag>*>(node);
  }

  static T* GetNode(IntrusiveHook<Tag>* hook) noexcept {
    // Safe downcast from base hook to derived node
    return static_cast<T*>(hook);
  }

  void InsertBetween(IntrusiveHook<Tag>* node, IntrusiveHook<Tag>* prev,
                     IntrusiveHook<Tag>* next) noexcept {
    node->next = next;
    node->prev = prev;
    prev->next = node;
    next->prev = node;
    Tracker::Inc();
  }

  void RemoveHook(IntrusiveHook<Tag>* node) noexcept {
    node->prev->next = node->next;
    node->next->prev = node->prev;

    // Reset the hook to unlinked circular state
    node->next = node;
    node->prev = node;
    Tracker::Dec();
  }
};

// Dummy tags for our fiber
struct AllFibersTag {};
struct ReadyQueueTag {};
struct WaitQueueTag {};
struct IdleQueueTag {};
struct TimerTag {};

}  // namespace tinyio
