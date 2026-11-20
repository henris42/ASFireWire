// Unit tests for the CPU idle limit (Audio/DriverKit/Runtime/BusStallLimit.hpp):
// only the kernel's named values are taken, a device asks for the limit set at
// its IO start and gives up what it holds once, and the holder count follows.

#include <gtest/gtest.h>

#include "Audio/DriverKit/Runtime/BusStallLimit.hpp"

namespace {

using ASFW::Audio::Runtime::BusStallHold;
using ASFW::Audio::Runtime::BusStallLimitNanos;
using ASFW::Audio::Runtime::BusStallWire;
using ASFW::Audio::Runtime::CopyBusStallState;
using ASFW::Audio::Runtime::BusStallHolders;
using ASFW::Audio::Runtime::IsBusStallLimit;
using ASFW::Audio::Runtime::kBusStallNone;
using ASFW::Audio::Runtime::kDefaultBusStallNanos;
using ASFW::Audio::Runtime::SetBusStallLimitNanos;

class BusStallLimitTests : public ::testing::Test {
protected:
    void TearDown() override { (void)SetBusStallLimitNanos(kDefaultBusStallNanos); }
};

TEST_F(BusStallLimitTests, TakesOnlyTheKernelsValues) {
    EXPECT_EQ(BusStallLimitNanos(), 5'000u);
    for (const uint64_t nanos : {0u, 5'000u, 10'000u, 20'000u, 25'000u, 30'000u, 40'000u}) {
        EXPECT_TRUE(IsBusStallLimit(nanos)) << nanos;
        EXPECT_TRUE(SetBusStallLimitNanos(nanos)) << nanos;
        EXPECT_EQ(BusStallLimitNanos(), nanos);
    }
    for (const uint64_t nanos : {1u, 5u, 4'999u, 15'000u, 50'000u, 40'001u}) {
        EXPECT_FALSE(SetBusStallLimitNanos(nanos)) << nanos;
        EXPECT_EQ(BusStallLimitNanos(), 40'000u);
    }
    // A value that would pass once truncated to 32 bits.
    EXPECT_FALSE(SetBusStallLimitNanos((uint64_t{1} << 32) + 5'000));
    EXPECT_EQ(BusStallLimitNanos(), 40'000u);
}

TEST_F(BusStallLimitTests, HoldFollowsTheSettingAtEachStart) {
    BusStallHold first;
    BusStallHold second;
    EXPECT_FALSE(first.End());

    EXPECT_EQ(first.Begin(), kDefaultBusStallNanos);
    EXPECT_EQ(BusStallHolders(), 1u);
    EXPECT_EQ(second.Begin(), kDefaultBusStallNanos);
    EXPECT_EQ(BusStallHolders(), 2u);

    // A new limit holds from the next start; a second start does not count twice.
    ASSERT_TRUE(SetBusStallLimitNanos(20'000));
    EXPECT_EQ(first.HeldNanos(), kDefaultBusStallNanos);
    EXPECT_EQ(first.Begin(), 20'000u);
    EXPECT_EQ(first.HeldNanos(), 20'000u);
    EXPECT_EQ(BusStallHolders(), 2u);

    EXPECT_TRUE(first.End());
    EXPECT_FALSE(first.End());
    EXPECT_EQ(BusStallHolders(), 1u);

    // Off: the start asks for none, and gives up what the device held.
    ASSERT_TRUE(SetBusStallLimitNanos(kBusStallNone));
    EXPECT_EQ(second.Begin(), kBusStallNone);
    EXPECT_EQ(BusStallHolders(), 0u);
    EXPECT_FALSE(second.End());
    EXPECT_EQ(first.Begin(), kBusStallNone);
    EXPECT_EQ(BusStallHolders(), 0u);
}

// Selectors 76 and 77 answer with the limit set and the devices holding one.
TEST_F(BusStallLimitTests, WireCarriesTheLimitAndTheHolders) {
    BusStallHold device;
    ASSERT_TRUE(SetBusStallLimitNanos(25'000));
    EXPECT_EQ(device.Begin(), 25'000u);

    BusStallWire state{};
    CopyBusStallState(state);
    EXPECT_EQ(state.limitNanos, 25'000u);
    EXPECT_EQ(state.holders, 1u);

    EXPECT_TRUE(device.End());
    CopyBusStallState(state);
    EXPECT_EQ(state.holders, 0u);
}

} // namespace
