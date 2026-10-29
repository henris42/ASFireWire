#include "Audio/Runtime/PlaybackRingRange.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using ASFW::Audio::Runtime::UpdatePlaybackRingRange;

TEST(PlaybackRingRangeTests, FirstWriteEstablishesActualValidStart) {
    const auto update = UpdatePlaybackRingRange(0, 0, 1000, 1192, 0, 2048);
    EXPECT_EQ(update.oldestValidFrame, 1000U);
    EXPECT_EQ(update.writtenEndFrame, 1192U);
    EXPECT_FALSE(update.discontinuity);
    EXPECT_FALSE(update.overrun);
}

TEST(PlaybackRingRangeTests, ContiguousWritesRetainOldestUntilCapacityWrap) {
    auto update = UpdatePlaybackRingRange(1192, 1000, 1192, 1320, 1100, 256);
    EXPECT_EQ(update.oldestValidFrame, 1064U);
    EXPECT_FALSE(update.discontinuity);

    update = UpdatePlaybackRingRange(1320, update.oldestValidFrame,
                                     1320, 1448, 1200, 256);
    EXPECT_EQ(update.oldestValidFrame, 1192U);
    EXPECT_FALSE(update.discontinuity);
}

TEST(PlaybackRingRangeTests, DiscontinuousWriteResetsRangeStart) {
    const auto update = UpdatePlaybackRingRange(1320, 1064, 2000, 2128, 1300, 512);
    EXPECT_EQ(update.oldestValidFrame, 2000U);
    EXPECT_EQ(update.writtenEndFrame, 2128U);
    EXPECT_TRUE(update.discontinuity);
    EXPECT_TRUE(update.overrun);
}

TEST(PlaybackRingRangeTests, RepeatedAndOverflowedWritesAreIgnored) {
    const auto repeat = UpdatePlaybackRingRange(100, 20, 40, 100, 50, 64);
    EXPECT_EQ(repeat.oldestValidFrame, 20U);
    EXPECT_EQ(repeat.writtenEndFrame, 100U);
    EXPECT_FALSE(repeat.rewound);

    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    const auto invalid = UpdatePlaybackRingRange(kMax - 4, kMax - 20,
                                                 kMax - 2, 1, kMax - 10, 64);
    EXPECT_EQ(invalid.writtenEndFrame, kMax - 4);
    EXPECT_FALSE(invalid.rewound);
}

// A write that ends behind the recorded end proves that end was never
// written: the range follows the write back instead of ignoring it.
TEST(PlaybackRingRangeTests, AWriteBehindTheEndRewindsTheRange) {
    const auto update = UpdatePlaybackRingRange(100, 20, 40, 80, 50, 64);
    EXPECT_TRUE(update.rewound);
    EXPECT_TRUE(update.discontinuity);
    EXPECT_EQ(update.writtenEndFrame, 80U);
    EXPECT_EQ(update.oldestValidFrame, 20U);
}

// b146: the first write after a cold start covered a whole ring while
// CoreAudio had written one buffer. The next write corrects the range, and
// its start bounds what is still known to be valid.
TEST(PlaybackRingRangeTests, AFirstWriteThatOverstatesItsSpanIsCorrectedByTheNext) {
    auto update = UpdatePlaybackRingRange(0, 0, 1000, 1000 + 2048, 0, 2048);
    EXPECT_EQ(update.writtenEndFrame, 3048U);
    EXPECT_EQ(update.oldestValidFrame, 1000U);

    update = UpdatePlaybackRingRange(update.writtenEndFrame, update.oldestValidFrame,
                                     1256, 1512, 1100, 2048);
    EXPECT_TRUE(update.rewound);
    EXPECT_EQ(update.writtenEndFrame, 1512U);
    EXPECT_EQ(update.oldestValidFrame, 1000U);

    // An end so far ahead that its capacity floor passed the write: the
    // write's own start is the oldest frame still known to be valid.
    update = UpdatePlaybackRingRange(9000, 7000, 1256, 1512, 1100, 2048);
    EXPECT_TRUE(update.rewound);
    EXPECT_EQ(update.oldestValidFrame, 1256U);
}

} // namespace
