// Unit tests for InterruptDispatcher::ShouldPublishStatus: isochronous
// completions republish the shared status at most every 250 ms, every other
// interrupt at once.

#include <gtest/gtest.h>

#include "../ASFWDriver/Hardware/InterruptDispatcher.hpp"
#include "../ASFWDriver/Hardware/RegisterMap.hpp"

namespace {

using ASFW::Driver::IntEventBits;
using ASFW::Driver::InterruptDispatcher;

constexpr uint64_t kMs = 1'000'000ULL;

TEST(InterruptStatusPublishTests, IsochronousCompletionsPublishAtMostEvery250ms) {
    InterruptDispatcher dispatcher;
    const uint32_t isoch = IntEventBits::kIsochTx | IntEventBits::kIsochRx;
    EXPECT_TRUE(dispatcher.ShouldPublishStatus(isoch, 1000 * kMs));
    EXPECT_FALSE(dispatcher.ShouldPublishStatus(IntEventBits::kIsochTx, 1001 * kMs));
    EXPECT_FALSE(dispatcher.ShouldPublishStatus(IntEventBits::kIsochRx, 1249 * kMs));
    EXPECT_TRUE(dispatcher.ShouldPublishStatus(isoch, 1250 * kMs));
    EXPECT_FALSE(dispatcher.ShouldPublishStatus(isoch, 1300 * kMs));
}

TEST(InterruptStatusPublishTests, AnyOtherEventPublishesAtOnce) {
    InterruptDispatcher dispatcher;
    EXPECT_TRUE(dispatcher.ShouldPublishStatus(IntEventBits::kIsochTx, 1000 * kMs));
    EXPECT_TRUE(dispatcher.ShouldPublishStatus(IntEventBits::kIsochTx | IntEventBits::kBusReset,
                                               1001 * kMs));
    EXPECT_TRUE(dispatcher.ShouldPublishStatus(IntEventBits::kRespTxComplete, 1002 * kMs));
    // And they do not hold back the next isochronous refresh.
    EXPECT_FALSE(dispatcher.ShouldPublishStatus(IntEventBits::kIsochRx, 1003 * kMs));
}

} // namespace
