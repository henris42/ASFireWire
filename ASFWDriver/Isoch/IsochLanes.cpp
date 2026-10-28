// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "IsochLanes.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Logging/Logging.hpp"

namespace ASFW::Driver {

uint32_t IsochLanes::LaneCount(HardwareInterface& hardware) noexcept {
    if (laneCount_ == 0) {
        const uint32_t contexts = hardware.ProbeIsochContextCount();
        const uint32_t usable = contexts > kMaxHardwareContexts ? kMaxHardwareContexts : contexts;
        laneCount_ = usable / kLaneWidth;
        if (laneCount_ == 0) laneCount_ = 1;
        ASFW_LOG(Isoch, "IsochLanes: controller offers %u isoch contexts per direction -> %u lanes of %u streams",
                 contexts, laneCount_, kLaneWidth);
    }
    return laneCount_;
}

IsochService* IsochLanes::Lane(uint32_t lane) noexcept {
    if (lane == 0) return &lane0_;
    if (lane >= kMaxLanes) return nullptr;
    return extra_[lane].get();
}

IsochService* IsochLanes::EnsureLane(uint32_t lane, HardwareInterface& hardware) noexcept {
    if (lane >= LaneCount(hardware)) return nullptr;
    if (lane == 0) return &lane0_;
    if (!extra_[lane]) {
        // Lane 0 ran the whole controller before another lane existed; its
        // secondary streams beyond the lane width sit on the contexts this
        // lane would take. Never create a lane under a running stream.
        if (lane0_.HasContextsFrom(kLaneWidth)) {
            ASFW_LOG_WARNING(Isoch, "IsochLanes: lane %u refused; lane 0 runs streams on contexts %u and up",
                             lane, kLaneWidth);
            return nullptr;
        }
        lane0_.SetStreamLimit(kLaneWidth);
        extra_[lane] = std::make_unique<IsochService>(static_cast<uint8_t>(lane * kLaneWidth));
        ASFW_LOG(Isoch, "IsochLanes: lane %u created on contexts %u..%u", lane, lane * kLaneWidth,
                 lane * kLaneWidth + kLaneWidth - 1);
    }
    return extra_[lane].get();
}

ASFW::Isoch::IsochReceiveContext* IsochLanes::ReceiveContext(uint32_t hardwareIndex) noexcept {
    const uint32_t lane = hardwareIndex / kLaneWidth;
    if (lane >= kMaxLanes) return nullptr;
    // Lane 0 keeps its legacy width over contexts no other lane has taken.
    if (lane == 0 || !extra_[lane]) return lane0_.ReceiveContext(hardwareIndex);
    return extra_[lane]->ReceiveContext(hardwareIndex % kLaneWidth);
}

ASFW::Isoch::IsochTransmitContext* IsochLanes::TransmitContext(uint32_t hardwareIndex) noexcept {
    const uint32_t lane = hardwareIndex / kLaneWidth;
    if (lane >= kMaxLanes) return nullptr;
    if (lane == 0 || !extra_[lane]) return lane0_.TransmitContext(hardwareIndex);
    return extra_[lane]->TransmitContext(hardwareIndex % kLaneWidth);
}

kern_return_t IsochLanes::StopAll() noexcept {
    kern_return_t result = lane0_.StopAll();
    for (auto& service : extra_) {
        if (service) {
            const kern_return_t kr = service->StopAll();
            if (kr != kIOReturnSuccess && result == kIOReturnSuccess) result = kr;
        }
    }
    return result;
}

} // namespace ASFW::Driver
