// Unit tests for InterruptDispatcher::PublishesStatusAtOnce: the app polls
// the shared status block and the watchdog rewrites it every 250 ms, so an
// interrupt writes it only for a bus reset or an unrecoverable error.

#include <gtest/gtest.h>

#include "../ASFWDriver/Hardware/InterruptDispatcher.hpp"
#include "../ASFWDriver/Hardware/RegisterMap.hpp"

namespace {

using ASFW::Driver::IntEventBits;
using ASFW::Driver::InterruptDispatcher;

TEST(InterruptStatusPublishTests, BusResetAndUnrecoverableErrorPublishAtOnce) {
    EXPECT_TRUE(InterruptDispatcher::PublishesStatusAtOnce(IntEventBits::kBusReset));
    EXPECT_TRUE(InterruptDispatcher::PublishesStatusAtOnce(IntEventBits::kIsochTx | IntEventBits::kBusReset));
    EXPECT_TRUE(InterruptDispatcher::PublishesStatusAtOnce(IntEventBits::kUnrecoverableError));
}

TEST(InterruptStatusPublishTests, IsochronousAndAsyncCompletionsLeaveItToTheWatchdog) {
    EXPECT_FALSE(InterruptDispatcher::PublishesStatusAtOnce(IntEventBits::kIsochTx | IntEventBits::kIsochRx));
    EXPECT_FALSE(InterruptDispatcher::PublishesStatusAtOnce(IntEventBits::kRespTxComplete));
    EXPECT_FALSE(InterruptDispatcher::PublishesStatusAtOnce(IntEventBits::kReqTxComplete | IntEventBits::kRQPkt |
                                                            IntEventBits::kRSPkt));
}

} // namespace
