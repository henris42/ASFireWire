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
//   kTransmitOnly      do not start the host receive context: if the device
//                      still resets, the receive side is out of the picture.
//   kHoldAbsolutePhase  hold the absolute SPH phase at the mk3 setpoint.
//   presentationLeadTicks  stamp the first data block this far ahead of its
//                      transmit cycle instead of the model's lead.
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
    // Hold the absolute SPH phase at the 828mk3 setpoint (+510) on a model
    // whose loop is relative by default (protocol v2 since b98).
    kMotuExperimentHoldAbsolutePhase = 1u << 2,
    // Start the host transmit context only: no receive context, so the
    // transmit cadence is the configured blocking one and the SPH clock
    // free-runs from the transmit time (no device clock to lock to, PCM
    // muted). Cleared by the session once its start has used it.
    kMotuExperimentTransmitOnly = 1u << 3,
    // Prime the transmit ring with 1512 header-only CIP packets carrying DBC 0
    // (every build before b110) instead of the default skip cycles, for A/B:
    // the 828mkII fresh from a power cycle aborted its stream on the first
    // data packet after that lap, and never after a skip-cycle lap (b107).
    kMotuExperimentEmptyCipPrefill = 1u << 4,
    // Count DBC as the device itself does on the wire: a data packet carries
    // its first block's number and an empty packet the next packet's
    // (IEC 61883-1 start-of-packet), instead of Linux's end-event counting.
    kMotuExperimentStartEventDbc = 1u << 5,
};
inline constexpr uint32_t kMotuExperimentAllFlags =
    kMotuExperimentForceFetchEnable | kMotuExperimentCaptureOnly | kMotuExperimentHoldAbsolutePhase |
    kMotuExperimentTransmitOnly | kMotuExperimentEmptyCipPrefill | kMotuExperimentStartEventDbc;

struct MotuExperiments {
    static inline std::atomic<uint32_t> flags{0};
    static inline std::atomic<uint32_t> extraNoDataPackets{0};
    // SPH presentation lead for the next start, in ticks (0: the model's).
    static inline std::atomic<uint32_t> presentationLeadTicks{0};

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
    [[nodiscard]] static uint32_t TakePresentationLeadTicks() noexcept {
        return presentationLeadTicks.exchange(0, std::memory_order_acq_rel);
    }
};

} // namespace ASFW::Audio::Motu
