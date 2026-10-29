// Unit tests for Hardware/DmaReach.hpp: which bus addresses the OHCI controller
// can use (below 4 GiB, outside excluded ranges) and the asfw-dma-exclude syntax.

#include "Hardware/DmaReach.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using ASFW::Driver::DmaReach::BusRange;
using ASFW::Driver::DmaReach::ExcludedHit;
using ASFW::Driver::DmaReach::kAddressLimit;
using ASFW::Driver::DmaReach::ParseExcludedRanges;
using ASFW::Driver::DmaReach::Reachable;

TEST(DmaReachTests, APageBelowFourGiBIsReachable) {
    EXPECT_TRUE(Reachable(0x67a6a000, 4096, {}));
    EXPECT_TRUE(Reachable(kAddressLimit - 4096, 4096, {}));
}

TEST(DmaReachTests, APageAtOrCrossingFourGiBIsNot) {
    EXPECT_FALSE(Reachable(kAddressLimit, 4096, {}));
    EXPECT_FALSE(Reachable(kAddressLimit - 2048, 4096, {}));
    EXPECT_FALSE(Reachable(0x3f0000000ull, 4096, {}));
    EXPECT_FALSE(Reachable(0x1000, 0, {}));
}

TEST(DmaReachTests, TheBridgeWindowFromTheEsxiHostIsExcluded) {
    const std::vector<BusRange> excluded{{0x90300000, 0x903fffff}};
    EXPECT_FALSE(Reachable(0x9034e000, 4096, excluded));
    EXPECT_FALSE(Reachable(0x90300000, 4096, excluded));
    EXPECT_FALSE(Reachable(0x903ff000, 4096, excluded));
    // The pages either side of the window are fine; a page straddling its edge is not.
    EXPECT_TRUE(Reachable(0x902ff000, 4096, excluded));
    EXPECT_TRUE(Reachable(0x90400000, 4096, excluded));
    EXPECT_FALSE(Reachable(0x902ff800, 4096, excluded));
    ASSERT_NE(ExcludedHit(0x902ff800, 4096, excluded), nullptr);
    EXPECT_EQ(ExcludedHit(0x902ff800, 4096, excluded)->first, 0x90300000u);
}

TEST(DmaReachTests, ExcludedRangesParse) {
    std::vector<BusRange> ranges;
    ASSERT_TRUE(ParseExcludedRanges("0x90300000-0x903fffff", ranges));
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].first, 0x90300000u);
    EXPECT_EQ(ranges[0].last, 0x903fffffu);

    ASSERT_TRUE(ParseExcludedRanges(" 90300000-903FFFFF, 0xA0000000-0xA00fffff ", ranges));
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[1].first, 0xa0000000u);
    EXPECT_EQ(ranges[1].last, 0xa00fffffu);

    ASSERT_TRUE(ParseExcludedRanges("", ranges));
    EXPECT_TRUE(ranges.empty());
}

TEST(DmaReachTests, AMalformedEntryRejectsTheWholeProperty) {
    std::vector<BusRange> ranges{{1, 2}};
    EXPECT_FALSE(ParseExcludedRanges("0x90300000", ranges));
    EXPECT_TRUE(ranges.empty());
    EXPECT_FALSE(ParseExcludedRanges("0x903fffff-0x90300000", ranges));
    EXPECT_FALSE(ParseExcludedRanges("0x90300000-0x903fffff, 0xzz-0x1", ranges));
    EXPECT_FALSE(ParseExcludedRanges("0x1-0x10000000000000000", ranges));
    EXPECT_TRUE(ranges.empty());
}

} // namespace
