#include "tinyio/intrusive_list.hpp"

#include <vector>

#include <gtest/gtest.h>

using namespace tinyio;

// ---------------------------------------------------------------------------
// Tags and test node type
// ---------------------------------------------------------------------------

namespace {
struct PrimaryTag {};

struct SecondaryTag {};

// Node that can live in two independent lists at the same time.
// Inheriting from both hooks lets us test multi-tag isolation.
struct Node : IntrusiveHook<PrimaryTag>, IntrusiveHook<SecondaryTag> {
  int value;
  explicit Node(const int v) : value(v) {}
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// Five pre-allocated nodes (values 0–4) and an empty list.
// Tests that only need a subset can ignore the unused nodes.
class IntrusiveListTest : public ::testing::Test {
 protected:
  Node n0_{0}, n1_{1}, n2_{2}, n3_{3}, n4_{4};
  IntrusiveList<Node, PrimaryTag> list_;
};
}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Drains a list into a vector of values via repeated pop_front.
template <typename Tag>
static std::vector<int> Drain(IntrusiveList<Node, Tag>& list) {
  std::vector<int> result;
  while (auto* n = list.PopFront()) {
    result.push_back(n->value);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Empty list
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, EmptyListInvariants) {
  EXPECT_TRUE(list_.Empty());
  EXPECT_EQ(list_.Size(), 0u);
  EXPECT_EQ(list_.PopFront(), nullptr);
}

// ---------------------------------------------------------------------------
// Single node
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, SingleNodeRoundtrip) {
  EXPECT_FALSE(n0_.IntrusiveHook<PrimaryTag>::IsLinked());

  list_.PushBack(&n0_);

  EXPECT_FALSE(list_.Empty());
  EXPECT_EQ(list_.Size(), 1u);
  EXPECT_TRUE(n0_.IntrusiveHook<PrimaryTag>::IsLinked());

  Node* popped = list_.PopFront();

  EXPECT_EQ(popped, &n0_);
  EXPECT_TRUE(list_.Empty());
  EXPECT_EQ(list_.Size(), 0u);
  EXPECT_FALSE(n0_.IntrusiveHook<PrimaryTag>::IsLinked());
  EXPECT_EQ(list_.PopFront(), nullptr);
}

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, PushBackGivesFifoOrder) {
  list_.PushBack(&n0_);
  list_.PushBack(&n1_);
  list_.PushBack(&n2_);
  list_.PushBack(&n3_);
  list_.PushBack(&n4_);

  EXPECT_EQ(Drain(list_), (std::vector{0, 1, 2, 3, 4}));
}

TEST_F(IntrusiveListTest, PushFrontReversesOrder) {
  list_.PushFront(&n0_);
  list_.PushFront(&n1_);
  list_.PushFront(&n2_);
  list_.PushFront(&n3_);
  list_.PushFront(&n4_);

  EXPECT_EQ(Drain(list_), (std::vector{4, 3, 2, 1, 0}));
}

TEST_F(IntrusiveListTest, MixedFrontAndBackInsertion) {
  list_.PushBack(&n0_);   // [0]
  list_.PushFront(&n1_);  // [1, 0]
  list_.PushBack(&n2_);   // [1, 0, 2]
  list_.PushFront(&n3_);  // [3, 1, 0, 2]

  EXPECT_EQ(Drain(list_), (std::vector{3, 1, 0, 2}));
}

// ---------------------------------------------------------------------------
// Size tracking
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, SizeTracksAllMutations) {
  list_.PushBack(&n0_);
  list_.PushBack(&n1_);
  list_.PushBack(&n2_);
  EXPECT_EQ(list_.Size(), 3u);

  (void)list_.PopFront();
  EXPECT_EQ(list_.Size(), 2u);

  list_.PushBack(&n3_);
  list_.PushBack(&n4_);
  EXPECT_EQ(list_.Size(), 4u);

  while (list_.PopFront()) {
  }
  EXPECT_EQ(list_.Size(), 0u);
  EXPECT_TRUE(list_.Empty());
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, RemoveMiddleElement) {
  list_.PushBack(&n0_);
  list_.PushBack(&n1_);
  list_.PushBack(&n2_);
  list_.PushBack(&n3_);
  list_.PushBack(&n4_);

  list_.Remove(&n2_);

  EXPECT_EQ(list_.Size(), 4u);
  EXPECT_FALSE(n2_.IntrusiveHook<PrimaryTag>::IsLinked());
  EXPECT_EQ(Drain(list_), (std::vector{0, 1, 3, 4}));
}

TEST_F(IntrusiveListTest, RemoveFrontElement) {
  list_.PushBack(&n0_);
  list_.PushBack(&n1_);
  list_.PushBack(&n2_);

  list_.Remove(&n0_);

  EXPECT_EQ(list_.Size(), 2u);
  EXPECT_EQ(Drain(list_), (std::vector{1, 2}));
}

TEST_F(IntrusiveListTest, RemoveBackElement) {
  list_.PushBack(&n0_);
  list_.PushBack(&n1_);
  list_.PushBack(&n2_);

  list_.Remove(&n2_);

  EXPECT_EQ(list_.Size(), 2u);
  EXPECT_EQ(Drain(list_), (std::vector{0, 1}));
}

TEST_F(IntrusiveListTest, RemoveNeverLinkedNodeIsNoOp) {
  list_.PushBack(&n0_);
  list_.Remove(&n1_);  // n1 was never pushed

  EXPECT_EQ(list_.Size(), 1u);
  EXPECT_EQ(Drain(list_), (std::vector{0}));
}

TEST_F(IntrusiveListTest, RemoveAlreadyUnlinkedNodeIsNoOp) {
  list_.PushBack(&n0_);
  list_.PushBack(&n1_);

  (void)list_.PopFront();  // unlinks n0
  list_.Remove(&n0_);      // n0 is already unlinked, must be a no-op

  EXPECT_EQ(list_.Size(), 1u);
  EXPECT_EQ(Drain(list_), (std::vector{1}));
}

// ---------------------------------------------------------------------------
// for_each
// ---------------------------------------------------------------------------

TEST_F(IntrusiveListTest, ForEachVisitsAllNodesInOrder) {
  list_.PushBack(&n0_);
  list_.PushBack(&n1_);
  list_.PushBack(&n2_);
  list_.PushBack(&n3_);
  list_.PushBack(&n4_);

  std::vector<int> visited;
  list_.ForEach([&](Node& node) { visited.push_back(node.value); });

  EXPECT_EQ(visited, (std::vector{0, 1, 2, 3, 4}));
  EXPECT_EQ(list_.Size(), 5u);  // for_each must not alter the list
}

TEST_F(IntrusiveListTest, ForEachSafeToRemoveCurrentNode) {
  // for_each saves `next` before invoking the callback, so removing the
  // current node from inside the callback is safe and must not skip nodes.
  list_.PushBack(&n0_);
  list_.PushBack(&n1_);
  list_.PushBack(&n2_);

  std::vector<int> visited;
  list_.ForEach([&](Node& node) {
    visited.push_back(node.value);
    list_.Remove(&node);
  });

  EXPECT_EQ(visited, (std::vector{0, 1, 2}));
  EXPECT_TRUE(list_.Empty());
}

// ---------------------------------------------------------------------------
// Multi-tag: one node, two independent lists
// ---------------------------------------------------------------------------

TEST(IntrusiveListMultiTagTest, NodeCanLiveInTwoListsSimultaneously) {
  Node a{10};
  Node b{20};
  Node c{30};
  IntrusiveList<Node, PrimaryTag> primary;
  IntrusiveList<Node, SecondaryTag> secondary;

  primary.PushBack(&a);
  primary.PushBack(&b);
  primary.PushBack(&c);
  secondary.PushBack(&b);  // b is now in both lists

  EXPECT_EQ(primary.Size(), 3u);
  EXPECT_EQ(secondary.Size(), 1u);

  // Removing b from primary must leave secondary untouched
  primary.Remove(&b);

  EXPECT_EQ(primary.Size(), 2u);
  EXPECT_EQ(secondary.Size(), 1u);
  EXPECT_FALSE(b.IntrusiveHook<PrimaryTag>::IsLinked());
  EXPECT_TRUE(b.IntrusiveHook<SecondaryTag>::IsLinked());

  EXPECT_EQ(Drain(primary), (std::vector{10, 30}));
  EXPECT_EQ(Drain(secondary), (std::vector{20}));
}
