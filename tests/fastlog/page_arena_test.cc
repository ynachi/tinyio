#include "fastlog/page_arena.hpp"

#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include "tinyio/runtime.hpp"

using namespace fastlog;

class PageArenaTest : public ::testing::Test {
 protected:
  static constexpr size_t k_page_size = 4096;
  static constexpr size_t k_num_pages = 4;
  static constexpr size_t k_memory_size = k_page_size * k_num_pages;

  tinyio::IoOptions io_opts_;
  std::unique_ptr<tinyio::IO> io_;

  void SetUp() override {
    io_ = std::make_unique<tinyio::IO>(
        0, io_opts_,
        std::vector<tinyio::BucketConfig>{{.size = k_memory_size, .count = 1}});
  }
};

TEST_F(PageArenaTest, InitializationAndBufferAccess) {
  PageArena arena(io_->GetBufferPool(), k_page_size, k_num_pages);
  EXPECT_EQ(arena.Buffer().Size(), k_memory_size);
  EXPECT_NE(arena.Buffer().Ptr(), nullptr);
}

TEST_F(PageArenaTest, WriteAndRead) {
  PageArena arena(io_->GetBufferPool(), k_page_size, k_num_pages);

  // Write some data
  std::array data = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                     std::byte{0x05}};
  arena.Write(100, data);

  // Read it back
  auto read_back = arena.Read(100, 5);
  EXPECT_EQ(read_back.size(), 5);
  EXPECT_EQ(std::memcmp(read_back.data(), data.data(), 5), 0);
}

TEST_F(PageArenaTest, ClearSlot) {
  PageArena arena(io_->GetBufferPool(), k_page_size, k_num_pages);

  // Write across two slots
  std::vector data(4096 * 2, std::byte{0xFF});
  arena.Write(0, data);

  // Clear slot 0
  arena.ClearSlot(0);

  // Slot 0 should be zeros
  auto read_slot0 = arena.Read(0, 4096);
  for (auto b : read_slot0) {
    EXPECT_EQ(b, std::byte{0});
  }

  // Slot 1 should still be 0xFF
  auto read_slot1 = arena.Read(4096, 4096);
  for (auto b : read_slot1) {
    EXPECT_EQ(b, std::byte{0xFF});
  }
}

TEST_F(PageArenaTest, AllocationFailure) {
  // Try to allocate more than the pool has (pool only has 1 buffer of k_memory_size)
  // Requesting double the size should throw
  EXPECT_THROW(PageArena(io_->GetBufferPool(), k_page_size, k_num_pages * 2),
               std::runtime_error);
}
