#pragma once

#include <cstddef>

namespace tinyio
{

/// \brief A generic hook for intrusive linked lists.
/// \tparam Tag A unique tag type to allow a single struct to be in multiple lists.
template <typename Tag>
struct IntrusiveHook
{
    // Initialize to point to itself (unlinked/circular state)
    IntrusiveHook* prev = this;
    IntrusiveHook* next = this;

    [[nodiscard]] bool is_linked() const noexcept { return next != this; }
};

/// \brief A branchless, circular, doubly-linked intrusive list using dummy heads.
/// \tparam T The node type (must publicly inherit from IntrusiveHook<Tag>).
/// \tparam Tag The unique tag identifying which hook to use.
template <typename T, typename Tag>
class IntrusiveList
{
public:
    constexpr IntrusiveList() noexcept = default;

    [[nodiscard]] bool empty() const noexcept { return dummy_.next == &dummy_; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /// \brief Pushes a node to the front of the list (O(1), branchless)
    void push_front(T* node) noexcept { insert_between(get_hook(node), &dummy_, dummy_.next); }

    /// \brief Pushes a node to the back of the list (O(1), branchless)
    /// This gives the list perfect FIFO Queue semantics.
    void push_back(T* node) noexcept { insert_between(get_hook(node), dummy_.prev, &dummy_); }

    /// \brief Pops a node from the front of the list (O(1), branchless)
    [[nodiscard]] T* pop_front() noexcept
    {
        if (empty())
            return nullptr;
        IntrusiveHook<Tag>* hook = dummy_.next;
        remove_hook(hook);
        return get_node(hook);
    }

    /// \brief Removes a specific node from the list (O(1), branchless)
    void remove(T* node) noexcept
    {
        IntrusiveHook<Tag>* hook = get_hook(node);
        if (hook->is_linked())
        {
            remove_hook(hook);
        }
    }

private:
    IntrusiveHook<Tag> dummy_;
    std::size_t size_ = 0;

    static IntrusiveHook<Tag>* get_hook(T* node) noexcept
    {
        // Safe upcast from derived to base hook
        return static_cast<IntrusiveHook<Tag>*>(node);
    }

    static T* get_node(IntrusiveHook<Tag>* hook) noexcept
    {
        // Safe downcast from base hook to derived node
        return static_cast<T*>(hook);
    }

    void insert_between(IntrusiveHook<Tag>* node, IntrusiveHook<Tag>* prev, IntrusiveHook<Tag>* next) noexcept
    {
        node->next = next;
        node->prev = prev;
        prev->next = node;
        next->prev = node;
        ++size_;
    }

    void remove_hook(IntrusiveHook<Tag>* node) noexcept
    {
        node->prev->next = node->next;
        node->next->prev = node->prev;

        // Reset the hook to unlinked circular state
        node->next = node;
        node->prev = node;
        --size_;
    }
};

// Dummy tags for our fiber
struct AllFibersTag
{
};
struct ReadyQueueTag
{
};

}  // namespace tinyio
