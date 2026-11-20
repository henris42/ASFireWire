// Link metrics: what the receive descriptor status and the CIP headers tell
// about packets damaged or lost on the bus.

#include <gtest/gtest.h>

#include "Audio/Engine/Direct/Rx/RxCipContinuity.hpp"
#include "Isoch/Core/IsochLinkCounters.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace {

using ASFW::AudioEngine::Direct::Rx::RxCipContinuity;
using ASFW::Isoch::ClassifyIsochRxStatus;
using ASFW::Isoch::IsochRxLengthConsistent;
using ASFW::Isoch::IsochRxLinkCounters;
using ASFW::Isoch::IsochRxStatusClass;

// A stored packet: timestamp quadlet, header quadlet with data_length, data.
std::vector<uint8_t> StoredPacket(uint16_t dataLength, size_t storedDataBytes) {
    std::vector<uint8_t> bytes(8 + storedDataBytes, 0);
    bytes[6] = static_cast<uint8_t>(dataLength & 0xFF);
    bytes[7] = static_cast<uint8_t>(dataLength >> 8);
    return bytes;
}

constexpr uint16_t kRunning = 0x8400; // run | active, as the controller stores them
constexpr uint16_t kS400 = 2U << 5;

uint64_t Load(const std::atomic<uint64_t>& counter) { return counter.load(); }

TEST(IsochLinkCounters, StatusClasses) {
    EXPECT_EQ(ClassifyIsochRxStatus(kRunning | kS400 | 0x11), IsochRxStatusClass::kComplete);
    EXPECT_EQ(ClassifyIsochRxStatus(0x12), IsochRxStatusClass::kComplete);
    EXPECT_EQ(ClassifyIsochRxStatus(kRunning | kS400 | 0x1D), IsochRxStatusClass::kDataCrc);
    EXPECT_EQ(ClassifyIsochRxStatus(0x02), IsochRxStatusClass::kLongPacket);
    EXPECT_EQ(ClassifyIsochRxStatus(0x05), IsochRxStatusClass::kOverrun);
    EXPECT_EQ(ClassifyIsochRxStatus(0x06), IsochRxStatusClass::kDescriptorRead);
    EXPECT_EQ(ClassifyIsochRxStatus(0x08), IsochRxStatusClass::kDataWrite);
    EXPECT_EQ(ClassifyIsochRxStatus(0x0A), IsochRxStatusClass::kTimeout);
    EXPECT_EQ(ClassifyIsochRxStatus(0x0E), IsochRxStatusClass::kUnknown);
    EXPECT_EQ(ClassifyIsochRxStatus(0x00), IsochRxStatusClass::kOther);
    EXPECT_EQ(ClassifyIsochRxStatus(0x1E), IsochRxStatusClass::kOther);
}

TEST(IsochLinkCounters, LengthAgainstTheHeader) {
    const auto exact = StoredPacket(16, 16);
    EXPECT_TRUE(IsochRxLengthConsistent(exact.data(), exact.size()));
    const auto padded = StoredPacket(13, 16); // pad bytes stored
    EXPECT_TRUE(IsochRxLengthConsistent(padded.data(), padded.size()));
    const auto unpadded = StoredPacket(13, 13);
    EXPECT_TRUE(IsochRxLengthConsistent(unpadded.data(), unpadded.size()));
    const auto cut = StoredPacket(624, 4); // header says more than arrived
    EXPECT_FALSE(IsochRxLengthConsistent(cut.data(), cut.size()));
    const auto extra = StoredPacket(16, 24);
    EXPECT_FALSE(IsochRxLengthConsistent(extra.data(), extra.size()));
    const std::array<uint8_t, 4> runt{};
    EXPECT_FALSE(IsochRxLengthConsistent(runt.data(), runt.size()));
    EXPECT_FALSE(IsochRxLengthConsistent(nullptr, 0));
}

TEST(IsochLinkCounters, CountsByStatusAndKeepsTheLastBadWord) {
    IsochRxLinkCounters counters;
    const auto good = StoredPacket(16, 16);
    for (int i = 0; i < 5; ++i) {
        counters.CountPacket(kRunning | kS400 | 0x11, good.data(), good.size());
    }
    counters.CountPacket(kRunning | kS400 | 0x1D, good.data(), good.size());
    counters.CountPacket(kRunning | kS400 | 0x02, good.data(), good.size());
    counters.CountPacket(kRunning | kS400 | 0x05, good.data(), good.size());
    counters.CountPacket(kRunning | kS400 | 0x07, good.data(), good.size());
    EXPECT_EQ(Load(counters.packets), 9U);
    EXPECT_EQ(Load(counters.dataCrc), 1U);
    EXPECT_EQ(Load(counters.longPacket), 1U);
    EXPECT_EQ(Load(counters.overrun), 1U);
    EXPECT_EQ(Load(counters.otherStatus), 1U);
    EXPECT_EQ(Load(counters.lengthMismatch), 0U);
    EXPECT_EQ(counters.lastBadStatus.load(), static_cast<uint32_t>(kRunning | kS400 | 0x07));
    EXPECT_EQ(counters.lastSpeedCode.load(), 2U);

    // A complete packet whose stored bytes disagree with its header.
    const auto cut = StoredPacket(624, 4);
    counters.CountPacket(kRunning | kS400 | 0x11, cut.data(), cut.size());
    EXPECT_EQ(Load(counters.lengthMismatch), 1U);
    // A damaged packet is not also a length error.
    counters.CountPacket(kRunning | kS400 | 0x1D, cut.data(), cut.size());
    EXPECT_EQ(Load(counters.lengthMismatch), 1U);
    EXPECT_EQ(Load(counters.dataCrc), 2U);

    counters.Reset();
    EXPECT_EQ(Load(counters.packets), 0U);
    EXPECT_EQ(Load(counters.dataCrc), 0U);
    EXPECT_EQ(counters.lastBadStatus.load(), 0U);
}

RxCipContinuity::Packet Cip(uint8_t dbc, uint32_t blocks, uint8_t dbs = 15, uint8_t sid = 2,
                            uint8_t fdf = 0x22) {
    return {.dbc = dbc, .dbs = dbs, .sid = sid, .fmt = 0x02, .fdf = fdf, .blocks = blocks};
}

// Feed `packets` packets by the rule, starting at `dbc`, alternating 12 and 13
// blocks with an empty packet now and then. Returns the next DBC.
uint8_t FeedByTheRule(RxCipContinuity& monitor, IsochRxLinkCounters& counters, uint8_t dbc,
                      uint32_t packets) {
    for (uint32_t i = 0; i < packets; ++i) {
        const uint32_t blocks = (i % 9 == 8) ? 0 : (i % 2 == 0 ? 12 : 13);
        monitor.Observe(Cip(dbc, blocks), counters);
        dbc = static_cast<uint8_t>(dbc + blocks);
    }
    return dbc;
}

TEST(RxCipContinuity, AStreamByTheRuleHasNoGaps) {
    RxCipContinuity monitor;
    IsochRxLinkCounters counters;
    (void)FeedByTheRule(monitor, counters, 250, 4000); // wraps at 256 many times
    EXPECT_EQ(Load(counters.dbcGaps), 0U);
    EXPECT_EQ(Load(counters.dbcBlocksMissed), 0U);
    EXPECT_EQ(Load(counters.cipChanges), 0U);
    EXPECT_EQ(counters.dbcUnusable.load(), 0U);
}

TEST(RxCipContinuity, ALostPacketIsOneGapOfItsBlocks) {
    RxCipContinuity monitor;
    IsochRxLinkCounters counters;
    uint8_t dbc = FeedByTheRule(monitor, counters, 0, 1000);
    dbc = static_cast<uint8_t>(dbc + 12); // a packet of 12 blocks never arrived
    dbc = FeedByTheRule(monitor, counters, dbc, 1000);
    EXPECT_EQ(Load(counters.dbcGaps), 1U);
    EXPECT_EQ(Load(counters.dbcBlocksMissed), 12U);

    // An empty packet carries the next block's count; one that does not is a gap.
    monitor.Observe(Cip(static_cast<uint8_t>(dbc + 13), 0), counters);
    EXPECT_EQ(Load(counters.dbcGaps), 2U);
    EXPECT_EQ(Load(counters.dbcBlocksMissed), 25U);
}

TEST(RxCipContinuity, AGapWithinTheFirstPacketsStillCounts) {
    RxCipContinuity monitor;
    IsochRxLinkCounters counters;
    uint8_t dbc = FeedByTheRule(monitor, counters, 0, 10);
    dbc = static_cast<uint8_t>(dbc + 13);
    (void)FeedByTheRule(monitor, counters, dbc, 10);
    EXPECT_EQ(Load(counters.dbcGaps), 1U);
    EXPECT_EQ(Load(counters.dbcBlocksMissed), 13U);
    EXPECT_EQ(counters.dbcUnusable.load(), 0U);
}

TEST(RxCipContinuity, ADeviceThatDoesNotCountByTheRuleIsMarkedUnusable) {
    RxCipContinuity monitor;
    IsochRxLinkCounters counters;
    // DBC stands still: every packet with data is a mismatch.
    for (uint32_t i = 0; i < 2000; ++i) {
        monitor.Observe(Cip(7, 12), counters);
    }
    EXPECT_EQ(counters.dbcUnusable.load(), 1U);
    EXPECT_EQ(Load(counters.dbcGaps), 0U);
    EXPECT_EQ(Load(counters.dbcBlocksMissed), 0U);

    monitor.Reset();
    counters.Reset();
    (void)FeedByTheRule(monitor, counters, 0, 600);
    EXPECT_EQ(counters.dbcUnusable.load(), 0U);
}

TEST(RxCipContinuity, HeaderFieldChangesCountOnPacketsWithData) {
    RxCipContinuity monitor;
    IsochRxLinkCounters counters;
    uint8_t dbc = FeedByTheRule(monitor, counters, 0, 100);
    // Empty packets with another DBS or FDF 0xFF are by design.
    monitor.Observe(Cip(dbc, 0, 2), counters);
    monitor.Observe(Cip(dbc, 0, 15, 2, 0xFF), counters);
    EXPECT_EQ(Load(counters.cipChanges), 0U);
    // The source node number changed (a bus reset renumbered the bus).
    monitor.Observe(Cip(dbc, 12, 15, 3), counters);
    EXPECT_EQ(Load(counters.cipChanges), 1U);
    dbc = static_cast<uint8_t>(dbc + 12);
    monitor.Observe(Cip(dbc, 12, 15, 3), counters);
    EXPECT_EQ(Load(counters.cipChanges), 1U);
    dbc = static_cast<uint8_t>(dbc + 12);
    monitor.Observe(Cip(dbc, 12, 19, 3), counters);
    EXPECT_EQ(Load(counters.cipChanges), 2U);
    EXPECT_EQ(Load(counters.dbcGaps), 0U);
}

} // namespace
