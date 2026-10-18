// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IsochSnoopConsumer.hpp - a receive consumer that logs what arrives on an
// isochronous channel, raw. Started through the user client's
// StartIsochReceive (selector 32: channel, packets to log) on a controller
// with nothing else to do: daisy-chain a second card behind the device and
// it sees, byte for byte, what the first card put on the wire -- the
// isochronous header the receiving link saw, then the CIP header and the
// data blocks. Hardware, 2026-10-18: every host-side view of the 828mkII
// stream equalled Linux and the device still reset; the wire was the one
// place left to look.

#pragma once

#include "../Core/IsochTypes.hpp"
#include "../../Logging/Logging.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ASFW::Isoch {

class IsochSnoopConsumer final : public IIsochReceiveConsumer {
public:
    static constexpr uint32_t kDefaultPacketsToLog = 32;
    static constexpr uint32_t kMaxPacketsToLog = 4096;

    void Arm(uint8_t channel, uint32_t packetsToLog) noexcept {
        channel_ = channel;
        toLog_ = packetsToLog == 0 ? kDefaultPacketsToLog
               : packetsToLog > kMaxPacketsToLog ? kMaxPacketsToLog : packetsToLog;
        logged_ = 0;
        packets_.store(0, std::memory_order_relaxed);
        bytes_.store(0, std::memory_order_relaxed);
        withData_.store(0, std::memory_order_relaxed);
        armed_.store(true, std::memory_order_release);
    }
    void Disarm() noexcept { armed_.store(false, std::memory_order_release); }
    [[nodiscard]] bool IsArmed() const noexcept { return armed_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t Packets() const noexcept { return packets_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t Bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t PacketsWithData() const noexcept { return withData_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint8_t Channel() const noexcept { return channel_; }

    void BeginReceiveBatch(const IsochReceiveBatch&) noexcept override {}

    void ConsumePacket(const IsochReceiveBatch&, const IsochReceivePacket& packet) noexcept override {
        packets_.fetch_add(1, std::memory_order_relaxed);
        bytes_.fetch_add(packet.payload.size(), std::memory_order_relaxed);
        // The receive path hands 8 bytes of OHCI prefix (timestamp and the
        // isochronous header quadlet) before the packet's own bytes; a CIP
        // header alone is 8 more.
        if (packet.payload.size() > 16) {
            withData_.fetch_add(1, std::memory_order_relaxed);
        }
        if (logged_ >= toLog_) {
            return;
        }
        ++logged_;
        char hex[16 * 9 + 1] = {};
        size_t used = 0;
        const size_t quadlets = packet.payload.size() / 4;
        for (size_t q = 0; q < quadlets && q < 16; ++q) {
            const uint8_t* b = packet.payload.data() + q * 4;
            const uint32_t value = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                                   (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
            used += static_cast<size_t>(std::snprintf(hex + used, sizeof(hex) - used, "%08x ", value));
        }
        ASFW_LOG(Isoch, "[IsochSnoop] ch=%u #%llu desc=%u status=0x%04x residual=%u bytes=%zu q=%{public}s",
                 channel_, static_cast<unsigned long long>(packets_.load(std::memory_order_relaxed)),
                 packet.descriptorIndex, packet.transferStatus, packet.residualCount, packet.payload.size(), hex);
    }

private:
    uint8_t channel_{0};
    uint32_t toLog_{kDefaultPacketsToLog};
    uint32_t logged_{0};
    std::atomic<bool> armed_{false};
    std::atomic<uint64_t> packets_{0};
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> withData_{0};
};

} // namespace ASFW::Isoch
