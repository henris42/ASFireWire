// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IsochLanes.hpp - one isoch service per streaming device on a controller.
//
// An OHCI controller has several IT and IR contexts (eight on the TI XIO2213,
// four at minimum). Lane L owns contexts [L * kLaneWidth, (L+1) * kLaneWidth):
// its IsochService runs a device's master stream on the first and a secondary
// on the second. Lane 0 is the controller's original service (ServiceContext::
// isoch, also used by DV capture and the channel snoop); the others are made
// on demand. Interrupts are routed by hardware context index to the lane that
// owns it. Two 828mkIIs on one card (2026-10-28) could stream only one at a
// time before this.

#pragma once

#include "IsochService.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace ASFW::Driver {

class IsochLanes final {
public:
    static constexpr uint32_t kLaneWidth = IsochService::kLaneWidth;
    static constexpr uint32_t kMaxHardwareContexts = 8;
    static constexpr uint32_t kMaxLanes = kMaxHardwareContexts / kLaneWidth;

    explicit IsochLanes(IsochService& lane0) noexcept : lane0_(lane0) {}
    IsochLanes(const IsochLanes&) = delete;
    IsochLanes& operator=(const IsochLanes&) = delete;

    // How many lanes this controller can run: its context count (probed once
    // from the hardware; four when the probe cannot run) over the lane width.
    [[nodiscard]] uint32_t LaneCount(HardwareInterface& hardware) noexcept;
    [[nodiscard]] uint32_t LaneCountIfKnown() const noexcept { return laneCount_ == 0 ? 1 : laneCount_; }

    // The lane's service; lanes beyond LaneCount are never created.
    [[nodiscard]] IsochService* Lane(uint32_t lane) noexcept;
    [[nodiscard]] IsochService* EnsureLane(uint32_t lane, HardwareInterface& hardware) noexcept;

    // The context a hardware index belongs to, for interrupt routing. Null
    // when the lane does not exist or has not created that context.
    [[nodiscard]] ASFW::Isoch::IsochReceiveContext* ReceiveContext(uint32_t hardwareIndex) noexcept;
    [[nodiscard]] ASFW::Isoch::IsochTransmitContext* TransmitContext(uint32_t hardwareIndex) noexcept;

    // Every lane's StopAll; the first failure is returned.
    [[nodiscard]] kern_return_t StopAll() noexcept;

private:
    IsochService& lane0_;
    std::array<std::unique_ptr<IsochService>, kMaxLanes> extra_{};
    uint32_t laneCount_{0};
};

} // namespace ASFW::Driver
