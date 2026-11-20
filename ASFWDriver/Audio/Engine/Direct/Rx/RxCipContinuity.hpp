// RxCipContinuity.hpp
// ASFW - packets a received CIP stream is missing, told from its own headers.
//
// IEC 61883-1 §6.2.1: DBC is the count of the packet's first data block, so a
// packet's DBC is the previous packet's DBC plus the blocks that packet
// carried, modulo 256; a packet without data carries the next block's count.
// A packet lost on the bus (or dropped by the device) shows as a step, and the
// step is the number of blocks that went with it.

#pragma once

#include "../../../../Isoch/Core/IsochLinkCounters.hpp"

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Rx {

class RxCipContinuity final {
public:
    struct Packet final {
        uint8_t dbc{0};
        uint8_t dbs{0};
        uint8_t sid{0};
        uint8_t fmt{0};
        uint8_t fdf{0};
        uint32_t blocks{0};
    };

    // The first packets of a stream decide whether the device counts by the
    // rule at all (some count the last block, some stand still on empty
    // packets). When it does not, what the trial counted is taken back and
    // the stream's counter is marked unusable.
    static constexpr uint32_t kTrialPackets = 256;
    static constexpr uint32_t kTrialMismatchLimit = 32;

    void Reset() noexcept { *this = RxCipContinuity{}; }

    void Observe(const Packet& packet, ::ASFW::Isoch::IsochRxLinkCounters& counters) noexcept {
        using Counters = ::ASFW::Isoch::IsochRxLinkCounters;
        // Header fields are compared between packets that carry data: empty
        // packets may differ by design (FDF 0xFF "no data"; the M-Audio 1814
        // idles with another DBS).
        if (packet.blocks != 0 && packet.fdf != 0xFF) {
            if (fieldsKnown_ && (packet.dbs != dbs_ || packet.sid != sid_ || packet.fmt != fmt_ ||
                                 packet.fdf != fdf_)) {
                Counters::Add(counters.cipChanges);
            }
            dbs_ = packet.dbs;
            sid_ = packet.sid;
            fmt_ = packet.fmt;
            fdf_ = packet.fdf;
            fieldsKnown_ = true;
        }

        if (unusable_) {
            return;
        }
        if (dbcKnown_) {
            const uint8_t expected = static_cast<uint8_t>(dbc_ + blocks_);
            const uint8_t step = static_cast<uint8_t>(packet.dbc - expected);
            const bool trial = checked_ < kTrialPackets;
            if (trial) {
                ++checked_;
            }
            if (step != 0) {
                Counters::Add(counters.dbcGaps);
                Counters::Add(counters.dbcBlocksMissed, step);
                if (trial) {
                    ++trialGaps_;
                    trialBlocks_ += step;
                    if (trialGaps_ >= kTrialMismatchLimit) {
                        unusable_ = true;
                        Counters::Add(counters.dbcGaps, 0ULL - trialGaps_);
                        Counters::Add(counters.dbcBlocksMissed, 0ULL - trialBlocks_);
                        counters.dbcUnusable.store(1, std::memory_order_relaxed);
                        return;
                    }
                }
            }
        }
        dbc_ = packet.dbc;
        blocks_ = static_cast<uint8_t>(packet.blocks);
        dbcKnown_ = true;
    }

private:
    bool fieldsKnown_{false};
    bool dbcKnown_{false};
    bool unusable_{false};
    uint8_t dbs_{0};
    uint8_t sid_{0};
    uint8_t fmt_{0};
    uint8_t fdf_{0};
    uint8_t dbc_{0};
    uint8_t blocks_{0};
    uint32_t checked_{0};
    uint32_t trialGaps_{0};
    uint64_t trialBlocks_{0};
};

} // namespace ASFW::AudioEngine::Direct::Rx
