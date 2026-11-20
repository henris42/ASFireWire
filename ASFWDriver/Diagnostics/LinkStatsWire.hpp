// LinkStatsWire.hpp
// ASFW - the link metrics' totals as the user client hands them out
// (selector 78, `asfw-config-agent --link-stats`): 64-bit little-endian words.
//
//   header      version, units, async nodes, PHY ports, PHY state
//   controller  cycleLost, cycleInconsistent, cycleTooLong, postedWriteErr,
//               regAccessFail, unrecoverableError, busResets, generation
//   per unit    guid, flags (1 rx runs, 2 tx runs, 4 dbc not checked),
//               rx speed code, rx packets, crc, other status, length,
//               dbc gaps, dbc blocks, cycles without a packet, cip changes,
//               last bad rx status, tx packets, tx status errors,
//               tx cycles missed, last bad tx status
//   per node    node, requests, ack err, busy, no ack, timeouts, other
//   per port    port | reg8 << 8 | reg9 << 16 | reg10 << 24 | reg11 << 32
//
// Units count from their stream's start, the rest from the driver's.
// ASFW/DriverConnector+LinkStats.swift decodes it.

#pragma once

#include "LinkReport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ASFW::Diagnostics::Link {

class StatsWire final {
  public:
    static constexpr uint64_t kVersion = 1;
    static constexpr size_t kHeaderWords = 5;
    static constexpr size_t kControllerWords = 8;
    static constexpr size_t kUnitWords = 16;
    static constexpr size_t kAsyncWords = 7;
    static constexpr size_t kMaxUnits = 8;
    static constexpr size_t kMaxAsyncNodes = 16;
    static constexpr size_t kMaxPhyPorts = 16;
    static constexpr size_t kMaxWords = kHeaderWords + kControllerWords + kMaxUnits * kUnitWords +
                                        kMaxAsyncNodes * kAsyncWords + kMaxPhyPorts;

    // PHY state word: the local PHY's port registers were read, were left
    // alone because a stream runs (a read takes the bus's time), or did not
    // answer.
    static constexpr uint64_t kPhyRead = 0;
    static constexpr uint64_t kPhySkippedStreaming = 1;
    static constexpr uint64_t kPhyUnreadable = 2;

    static constexpr uint64_t kUnitRxRuns = 1;
    static constexpr uint64_t kUnitTxRuns = 2;
    static constexpr uint64_t kUnitDbcNotChecked = 4;

    StatsWire() noexcept { words_[0] = kVersion; }

    void SetController(const ControllerFigures& f) noexcept {
        uint64_t* w = &words_[kHeaderWords];
        w[0] = f.cycleLost;
        w[1] = f.cycleInconsistent;
        w[2] = f.cycleTooLong;
        w[3] = f.postedWriteErr;
        w[4] = f.regAccessFail;
        w[5] = f.unrecoverableError;
        w[6] = f.busResets;
        w[7] = f.generation;
    }

    // Units first, then nodes, then ports: each kind follows the one before.
    bool AddUnit(uint64_t guid, bool rxRuns, bool txRuns, const RxFigures& rx, const TxFigures& tx) noexcept {
        if (words_[1] >= kMaxUnits || words_[2] != 0 || words_[3] != 0) {
            return false;
        }
        uint64_t* w = &words_[used_];
        w[0] = guid;
        w[1] = (rxRuns ? kUnitRxRuns : 0) | (txRuns ? kUnitTxRuns : 0) | (rx.dbcUnusable ? kUnitDbcNotChecked : 0);
        w[2] = rx.speedCode;
        w[3] = rx.packets;
        w[4] = rx.dataCrc;
        w[5] = rx.otherStatus;
        w[6] = rx.lengthMismatch;
        w[7] = rx.dbcGaps;
        w[8] = rx.dbcBlocksMissed;
        w[9] = rx.cyclesMissed;
        w[10] = rx.cipChanges;
        w[11] = rx.lastBadStatus;
        w[12] = tx.packets;
        w[13] = tx.statusErrors;
        w[14] = tx.cyclesMissed;
        w[15] = tx.lastBadStatus;
        used_ += kUnitWords;
        ++words_[1];
        return true;
    }

    bool AddAsyncNode(uint32_t node, const AsyncNodeFigures& f) noexcept {
        if (words_[2] >= kMaxAsyncNodes || words_[3] != 0) {
            return false;
        }
        uint64_t* w = &words_[used_];
        w[0] = node;
        w[1] = f.requests;
        w[2] = f.ackErr;
        w[3] = f.busy;
        w[4] = f.noAck;
        w[5] = f.timeouts;
        w[6] = f.otherEvent;
        used_ += kAsyncWords;
        ++words_[2];
        return true;
    }

    bool AddPhyPort(uint8_t port, uint8_t reg8, uint8_t reg9, uint8_t reg10, uint8_t reg11) noexcept {
        if (words_[3] >= kMaxPhyPorts) {
            return false;
        }
        words_[used_] = static_cast<uint64_t>(port) | static_cast<uint64_t>(reg8) << 8 |
                        static_cast<uint64_t>(reg9) << 16 | static_cast<uint64_t>(reg10) << 24 |
                        static_cast<uint64_t>(reg11) << 32;
        ++used_;
        ++words_[3];
        return true;
    }

    void SetPhyState(uint64_t state) noexcept { words_[4] = state; }

    [[nodiscard]] const uint64_t* Words() const noexcept { return words_.data(); }
    [[nodiscard]] size_t WordCount() const noexcept { return used_; }
    [[nodiscard]] size_t ByteCount() const noexcept { return used_ * sizeof(uint64_t); }

  private:
    std::array<uint64_t, kMaxWords> words_{};
    size_t used_{kHeaderWords + kControllerWords};
};

} // namespace ASFW::Diagnostics::Link
