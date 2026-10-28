// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IsochLanesTests.cpp - lane creation and interrupt routing by hardware context.

#include <gtest/gtest.h>

#include "Hardware/HardwareInterface.hpp"
#include "Isoch/IsochLanes.hpp"
#include "Isoch/IsochService.hpp"

using ASFW::Driver::HardwareInterface;
using ASFW::Driver::IsochLanes;
using ASFW::Driver::IsochService;

// The host stub reports four isoch contexts per direction: four lanes of one.
TEST(IsochLanesTests, LaneCountComesFromTheControllerContextProbe) {
    IsochService isoch;
    IsochLanes lanes{isoch};
    HardwareInterface hardware{};
    EXPECT_EQ(lanes.LaneCountIfKnown(), 1U);
    EXPECT_EQ(lanes.LaneCount(hardware), 4U);
    EXPECT_EQ(lanes.LaneCountIfKnown(), 4U);
    EXPECT_EQ(lanes.Lane(0), &isoch);
    EXPECT_EQ(lanes.Lane(1), nullptr);
    EXPECT_EQ(lanes.EnsureLane(4, hardware), nullptr);
    EXPECT_EQ(lanes.EnsureLane(IsochLanes::kMaxLanes, hardware), nullptr);
}

TEST(IsochLanesTests, ExtraLanesSitAboveLaneZeroAndNarrowIt) {
    IsochService isoch;
    IsochLanes lanes{isoch};
    HardwareInterface hardware{};
    EXPECT_EQ(isoch.ContextBase(), 0U);
    EXPECT_EQ(isoch.StreamLimit(), IsochService::kMaxStreamsPerDirection);

    IsochService* lane1 = lanes.EnsureLane(1, hardware);
    ASSERT_NE(lane1, nullptr);
    EXPECT_EQ(lanes.EnsureLane(1, hardware), lane1);
    EXPECT_EQ(lanes.Lane(1), lane1);
    EXPECT_EQ(lane1->ContextBase(), IsochLanes::kLaneWidth);
    EXPECT_EQ(lane1->StreamLimit(), IsochLanes::kLaneWidth);
    // Lane 0 no longer owns the contexts the new lane runs on.
    EXPECT_EQ(isoch.StreamLimit(), IsochLanes::kLaneWidth);
}

// No contexts are created until a stream starts, so routing a hardware index
// finds nothing; it must still pick the right lane without touching others.
TEST(IsochLanesTests, HardwareIndexRoutesToTheLaneOwningIt) {
    IsochService isoch;
    IsochLanes lanes{isoch};
    HardwareInterface hardware{};
    for (uint32_t index = 0; index < IsochLanes::kMaxHardwareContexts; ++index) {
        EXPECT_EQ(lanes.ReceiveContext(index), nullptr);
        EXPECT_EQ(lanes.TransmitContext(index), nullptr);
    }
    EXPECT_EQ(lanes.ReceiveContext(IsochLanes::kMaxHardwareContexts), nullptr);
    ASSERT_NE(lanes.EnsureLane(1, hardware), nullptr);
    for (uint32_t index = 0; index < IsochLanes::kMaxHardwareContexts; ++index) {
        EXPECT_EQ(lanes.ReceiveContext(index), nullptr);
        EXPECT_EQ(lanes.TransmitContext(index), nullptr);
    }
    EXPECT_EQ(lanes.StopAll(), kIOReturnSuccess);
}
