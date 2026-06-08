#include "tinyio/intrusive_list.hpp"

#include <vector>

#include <gtest/gtest.h>

using namespace tinyio;

// ---------------------------------------------------------------------------
// Tags and test node type
// ---------------------------------------------------------------------------

namespace
{
struct PrimaryTag
{
};

struct SecondaryTag
{
};

// Node that can live in two independent lists at the same time.
// Inheriting from both hooks lets us test multi-tag isolation.
struct Node : IntrusiveHook<PrimaryTag>, IntrusiveHook<SecondaryTag>
{
    int value;
    explicit Node(int v) : value(v) {}
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// Five pre-allocated nodes (values 0–4) and an empty list.
// Tests that only need a subset can ignore the unused nodes.
class IntrusiveListTest : public ::testing::Test
{
protected:
    Node n0{0}, n1{1}, n2{2}, n3{3}, n4{4};
    IntrusiveList<Node, PrimaryTag> list_;
};
}  // namespace


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Drains a list into a vector of values via repeated pop_front.
template <typename Tag>
static std::vector<int> drain(IntrusiveList<Node, Tag>& list)
{
    std::vector<int> result;
    while (auto* n = list.pop_front())
        result.push_back(n->value);
    return result;
}

// ---------------------------------------------------------------------------
// Empty list
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, EmptyListInvariants)
{
    EXPECT_TRUE(list_.empty());
    EXPECT_EQ(list_.size(), 0u);
    EXPECT_EQ(list_.pop_front(), nullptr);
}

// ---------------------------------------------------------------------------
// Single node
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, SingleNodeRoundtrip)
{
    EXPECT_FALSE(n0.IntrusiveHook<PrimaryTag>::is_linked());

    list_.push_back(&n0);

    EXPECT_FALSE(list_.empty());
    EXPECT_EQ(list_.size(), 1u);
    EXPECT_TRUE(n0.IntrusiveHook<PrimaryTag>::is_linked());

    Node* popped = list_.pop_front();

    EXPECT_EQ(popped, &n0);
    EXPECT_TRUE(list_.empty());
    EXPECT_EQ(list_.size(), 0u);
    EXPECT_FALSE(n0.IntrusiveHook<PrimaryTag>::is_linked());
    EXPECT_EQ(list_.pop_front(), nullptr);
}

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, PushBackGivesFifoOrder)
{
    list_.push_back(&n0);
    list_.push_back(&n1);
    list_.push_back(&n2);
    list_.push_back(&n3);
    list_.push_back(&n4);

    EXPECT_EQ(drain(list_), (std::vector{0, 1, 2, 3, 4}));
}

TEST_F(IntrusiveListTest, PushFrontReversesOrder)
{
    list_.push_front(&n0);
    list_.push_front(&n1);
    list_.push_front(&n2);
    list_.push_front(&n3);
    list_.push_front(&n4);

    EXPECT_EQ(drain(list_), (std::vector{4, 3, 2, 1, 0}));
}

TEST_F(IntrusiveListTest, MixedFrontAndBackInsertion)
{
    list_.push_back(&n0);   // [0]
    list_.push_front(&n1);  // [1, 0]
    list_.push_back(&n2);   // [1, 0, 2]
    list_.push_front(&n3);  // [3, 1, 0, 2]

    EXPECT_EQ(drain(list_), (std::vector{3, 1, 0, 2}));
}

// ---------------------------------------------------------------------------
// Size tracking
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, SizeTracksAllMutations)
{
    list_.push_back(&n0);
    list_.push_back(&n1);
    list_.push_back(&n2);
    EXPECT_EQ(list_.size(), 3u);

    list_.pop_front();
    EXPECT_EQ(list_.size(), 2u);

    list_.push_back(&n3);
    list_.push_back(&n4);
    EXPECT_EQ(list_.size(), 4u);

    while (list_.pop_front()) {}
    EXPECT_EQ(list_.size(), 0u);
    EXPECT_TRUE(list_.empty());
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, RemoveMiddleElement)
{
    list_.push_back(&n0);
    list_.push_back(&n1);
    list_.push_back(&n2);
    list_.push_back(&n3);
    list_.push_back(&n4);

    list_.remove(&n2);

    EXPECT_EQ(list_.size(), 4u);
    EXPECT_FALSE(n2.IntrusiveHook<PrimaryTag>::is_linked());
    EXPECT_EQ(drain(list_), (std::vector{0, 1, 3, 4}));
}

TEST_F(IntrusiveListTest, RemoveFrontElement)
{
    list_.push_back(&n0);
    list_.push_back(&n1);
    list_.push_back(&n2);

    list_.remove(&n0);

    EXPECT_EQ(list_.size(), 2u);
    EXPECT_EQ(drain(list_), (std::vector{1, 2}));
}

TEST_F(IntrusiveListTest, RemoveBackElement)
{
    list_.push_back(&n0);
    list_.push_back(&n1);
    list_.push_back(&n2);

    list_.remove(&n2);

    EXPECT_EQ(list_.size(), 2u);
    EXPECT_EQ(drain(list_), (std::vector{0, 1}));
}

TEST_F(IntrusiveListTest, RemoveNeverLinkedNodeIsNoOp)
{
    list_.push_back(&n0);
    list_.remove(&n1);  // n1 was never pushed

    EXPECT_EQ(list_.size(), 1u);
    EXPECT_EQ(drain(list_), (std::vector{0}));
}

TEST_F(IntrusiveListTest, RemoveAlreadyUnlinkedNodeIsNoOp)
{
    list_.push_back(&n0);
    list_.push_back(&n1);

    list_.pop_front();   // unlinks n0
    list_.remove(&n0);   // n0 is already unlinked, must be a no-op

    EXPECT_EQ(list_.size(), 1u);
    EXPECT_EQ(drain(list_), (std::vector{1}));
}

// ---------------------------------------------------------------------------
// for_each
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, ForEachVisitsAllNodesInOrder)
{
    list_.push_back(&n0);
    list_.push_back(&n1);
    list_.push_back(&n2);
    list_.push_back(&n3);
    list_.push_back(&n4);

    std::vector<int> visited;
    list_.for_each([&](Node& node) { visited.push_back(node.value); });

    EXPECT_EQ(visited, (std::vector{0, 1, 2, 3, 4}));
    EXPECT_EQ(list_.size(), 5u);  // for_each must not alter the list
}

TEST_F(IntrusiveListTest, ForEachSafeToRemoveCurrentNode)
{
    // for_each saves `next` before invoking the callback, so removing the
    // current node from inside the callback is safe and must not skip nodes.
    list_.push_back(&n0);
    list_.push_back(&n1);
    list_.push_back(&n2);

    std::vector<int> visited;
    list_.for_each([&](Node& node) {
        visited.push_back(node.value);
        list_.remove(&node);
    });

    EXPECT_EQ(visited, (std::vector{0, 1, 2}));
    EXPECT_TRUE(list_.empty());
}

// ---------------------------------------------------------------------------
// Multi-tag: one node, two independent lists
// ---------------------------------------------------------------------------

TEST(IntrusiveListMultiTagTest, NodeCanLiveInTwoListsSimultaneously)
{
    Node a{10}, b{20}, c{30};
    IntrusiveList<Node, PrimaryTag>   primary;
    IntrusiveList<Node, SecondaryTag> secondary;

    primary.push_back(&a);
    primary.push_back(&b);
    primary.push_back(&c);
    secondary.push_back(&b);  // b is now in both lists

    EXPECT_EQ(primary.size(), 3u);
    EXPECT_EQ(secondary.size(), 1u);

    // Removing b from primary must leave secondary untouched
    primary.remove(&b);

    EXPECT_EQ(primary.size(), 2u);
    EXPECT_EQ(secondary.size(), 1u);
    EXPECT_FALSE(b.IntrusiveHook<PrimaryTag>::is_linked());
    EXPECT_TRUE(b.IntrusiveHook<SecondaryTag>::is_linked());

    EXPECT_EQ(drain(primary), (std::vector{10, 30}));
    EXPECT_EQ(drain(secondary), (std::vector{20}));
}
