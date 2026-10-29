// Unit tests for DMAMemoryManager::LayoutSize: a slab sized with it holds
// every region the allocator places, alignment padding included. The async
// slab used to be sized as the plain sum of its regions, so ResponseSender's
// lock scratch, allocated last, never fit (b175: "AllocateRegion would
// overflow - need 1024 (align pad 0), have 0" at every start).

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <numeric>

#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Shared/Memory/DMAMemoryManager.hpp"

namespace {

using ASFW::Driver::HardwareInterface;
using ASFW::Shared::DMAMemoryManager;

// The async subsystem's regions in allocation order: AT request and response
// descriptors, AR request and response descriptors, the AR buffers, and the
// lock-response scratch.
constexpr std::array<size_t, 7> kAsyncRegions = {4096, 1024, 2048, 4096, 524288, 1048576, 1024};

TEST(DMASlabLayoutTests, CountsTheAllocatorsPadding) {
    // A region that fits in a page is aligned to its own size, a larger one to a page.
    EXPECT_EQ(DMAMemoryManager::LayoutSize({1024, 2048}), 4096u);
    EXPECT_EQ(DMAMemoryManager::LayoutSize({16, 4097}), 4096u + 4112u);
    EXPECT_EQ(DMAMemoryManager::LayoutSize({3, 3}), 32u);
    EXPECT_EQ(DMAMemoryManager::LayoutSize({4096, 1024, 2048, 4096, 524288, 1048576, 1024}), 1586176u);
    EXPECT_GT(DMAMemoryManager::LayoutSize({4096, 1024, 2048, 4096, 524288, 1048576, 1024}),
              std::accumulate(kAsyncRegions.begin(), kAsyncRegions.end(), size_t{0}));
}

TEST(DMASlabLayoutTests, ASlabOfThatSizeHoldsEveryRegion) {
    HardwareInterface hw;
    DMAMemoryManager dma;
    ASSERT_TRUE(dma.Initialize(hw, DMAMemoryManager::LayoutSize(
                                       {4096, 1024, 2048, 4096, 524288, 1048576, 1024})));
    for (const size_t size : kAsyncRegions) {
        EXPECT_TRUE(dma.AllocateRegion(size).has_value()) << size;
    }
}

TEST(DMASlabLayoutTests, ThePlainSumLeavesTheLastRegionOut) {
    HardwareInterface hw;
    DMAMemoryManager dma;
    ASSERT_TRUE(dma.Initialize(hw, std::accumulate(kAsyncRegions.begin(), kAsyncRegions.end(), size_t{0})));
    for (size_t index = 0; index + 1 < kAsyncRegions.size(); ++index) {
        ASSERT_TRUE(dma.AllocateRegion(kAsyncRegions[index]).has_value()) << index;
    }
    EXPECT_FALSE(dma.AllocateRegion(kAsyncRegions.back()).has_value());
}

} // namespace
