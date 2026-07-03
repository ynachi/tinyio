#pragma once

#include <cstddef>

#include <boost/intrusive/list.hpp>

namespace tinyio {

template <typename Tag>
struct IntrusiveHook : boost::intrusive::list_base_hook<
                           boost::intrusive::tag<Tag>,
                           boost::intrusive::link_mode<boost::intrusive::auto_unlink>> {
  [[nodiscard]] bool IsLinked() const noexcept { return this->is_linked(); }

  void UnlinkSelf() noexcept {
    if (this->is_linked()) {
      this->unlink();
    }
  }
};

template <typename T, typename Tag, bool TrackSize = true>
class IntrusiveList {
  using ListType =
      boost::intrusive::list<T, boost::intrusive::base_hook<IntrusiveHook<Tag>>,
                             boost::intrusive::constant_time_size<false>>;

  ListType list_;
  std::size_t size_ = 0;

 public:
  constexpr IntrusiveList() noexcept = default;

  [[nodiscard]] bool Empty() const noexcept { return list_.empty(); }

  [[nodiscard]] std::size_t Size() const noexcept
    requires TrackSize
  {
    return size_;
  }

  void PushFront(T* node) noexcept {
    list_.push_front(*node);
    if constexpr (TrackSize) {
      ++size_;
    }
  }

  void PushBack(T* node) noexcept {
    list_.push_back(*node);
    if constexpr (TrackSize) {
      ++size_;
    }
  }

  [[nodiscard]] T* PopFront() noexcept {
    if (list_.empty()) {
      return nullptr;
    }
    T* front = &list_.front();
    list_.pop_front();
    if constexpr (TrackSize) {
      --size_;
    }
    return front;
  }

  void Remove(T* node) noexcept {
    if (static_cast<IntrusiveHook<Tag>*>(node)->IsLinked()) {
      list_.erase(list_.iterator_to(*node));
      if constexpr (TrackSize) {
        --size_;
      }
    }
  }

  template <class Fn>
  void ForEach(Fn&& fn) noexcept {
    for (auto it = list_.begin(); it != list_.end();) {
      auto& node = *it;
      ++it;
      fn(node);
    }
  }
};

// Dummy tags for our fiber
struct AllFibersTag {};
struct ReadyQueueTag {};
struct WaitQueueTag {};
struct IdleQueueTag {};
struct TimerTag {};

}  // namespace tinyio
