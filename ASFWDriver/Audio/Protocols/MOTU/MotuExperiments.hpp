// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuExperiments.hpp - diagnostic knobs for the 828mkII bus-reset
// investigation (hardware, 2026-10-18: the device initiates a bus reset on
// the first packet of ours that carries data). Set by the app through the
// user client (selector 71), process-wide, never persisted, consumed by the
// next stream start (one-shot). Each knob isolates one hypothesis:
//
//   kForceFetchEnable  write the clock register's fetch-enable bit before
//                      the start on models Linux exempts (828mk2, 896HD);
//                      the device's notification word drops that bit right
//                      before it resets.
//   kCaptureOnly       do not start the host transmit context at all: if the
//                      device still resets, our packets are exonerated.
//   extraNoDataPackets keep sending NO-DATA packets for this many packets
//                      after the prefill, so the first data packet moves by
//                      N x 125 us: a reset that moves with it is about the
//                      data; one that stays is a handshake/timing event.

#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Motu {

enum MotuExperimentFlag : uint32_t {
    kMotuExperimentForceFetchEnable = 1u << 0,
    kMotuExperimentCaptureOnly = 1u << 1,
};

struct MotuExperiments {
    static inline std::atomic<uint32_t> flags{0};
    static inline std::atomic<uint32_t> extraNoDataPackets{0};

    [[nodiscard]] static bool Has(MotuExperimentFlag flag) noexcept {
        return (flags.load(std::memory_order_relaxed) & flag) != 0;
    }
    // Each knob is one-shot: the start that uses it clears it, so a later
    // plain start is plain again (b92 report: a sticky hold surprised the
    // next afplay).
    [[nodiscard]] static bool Take(MotuExperimentFlag flag) noexcept {
        return (flags.fetch_and(~static_cast<uint32_t>(flag), std::memory_order_acq_rel) & flag) != 0;
    }
    [[nodiscard]] static uint32_t TakeExtraNoDataPackets() noexcept {
        return extraNoDataPackets.exchange(0, std::memory_order_acq_rel);
    }
};

} // namespace ASFW::Audio::Motu
