// The [Link] lines: what moved between two readings of the counters, which of
// it counts as an error, and the text.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Diagnostics/LinkReport.hpp"
#include "Diagnostics/LinkStatsWire.hpp"

namespace {

using namespace ASFW::Diagnostics::Link;

TEST(LinkReport, CountsSinceTheLastReadingAndAcrossARestart) {
    EXPECT_EQ(Since(120, 100), 20U);
    EXPECT_EQ(Since(100, 100), 0U);
    // A stream start zeroed the counter: what it holds is the interval's.
    EXPECT_EQ(Since(7, 100), 7U);
}

TEST(LinkReport, ReceiveFiguresComeFromTheCountersAndAddUpTheRareStatuses) {
    ASFW::Isoch::IsochRxLinkCounters counters;
    counters.packets.store(40000);
    counters.dataCrc.store(2);
    counters.longPacket.store(1);
    counters.overrun.store(1);
    counters.unknown.store(3);
    counters.dbcGaps.store(4);
    counters.dbcBlocksMissed.store(32);
    counters.cyclesMissed.store(5);
    counters.lastBadStatus.store(0x5D);
    counters.lastSpeedCode.store(2);

    const auto figures = RxFigures::From(counters);
    EXPECT_EQ(figures.packets, 40000U);
    EXPECT_EQ(figures.dataCrc, 2U);
    EXPECT_EQ(figures.otherStatus, 5U);
    EXPECT_EQ(figures.dbcGaps, 4U);
    EXPECT_TRUE(figures.AnyError());

    RxFigures before = figures;
    counters.packets.store(80000);
    const auto quiet = RxFigures::From(counters).Since(before);
    EXPECT_EQ(quiet.packets, 40000U);
    EXPECT_FALSE(quiet.AnyError());
}

TEST(LinkReport, ADeviceThatCountsDifferentlyHasNoDbcError) {
    ASFW::Isoch::IsochRxLinkCounters counters;
    counters.packets.store(8000);
    counters.dbcGaps.store(900);
    counters.dbcBlocksMissed.store(7000);
    counters.dbcUnusable.store(1);

    const auto figures = RxFigures::From(counters);
    EXPECT_TRUE(figures.dbcUnusable);
    EXPECT_EQ(figures.dbcGaps, 0U);
    EXPECT_FALSE(figures.AnyError());

    char line[224];
    FormatRx(line, sizeof(line), 0x0001F2000001F968ULL, "last 5.0 s", figures);
    EXPECT_NE(std::strstr(line, "dbc n/a"), nullptr);
}

TEST(LinkReport, ReceiveLine) {
    RxFigures figures;
    figures.packets = 40000;
    figures.speedCode = 2;
    char line[224];
    FormatRx(line, sizeof(line), 0x0001F2000001F968ULL, "last 5.0 s", figures);
    EXPECT_STREQ(line, "[Link] 0x0001f2000001f968 last 5.0 s: rx 40000 pk S400, crc 0, other status 0, length 0, "
                       "dbc gaps 0 (0 blocks), cycles missed 0, cip changes 0");

    figures.dataCrc = 3;
    figures.lastBadStatus = 0x005D;
    FormatRx(line, sizeof(line), 0x0001F2000001F968ULL, "stream", figures);
    EXPECT_NE(std::strstr(line, "stream: rx 40000 pk S400, crc 3, other status 0 (last 0x005d), length 0"), nullptr);
}

TEST(LinkReport, EveryLineFitsTheLogRingWithItsLargestNumbers) {
    // The ring keeps 232 bytes of a message, "[Metrics] " included.
    constexpr size_t kRoom = 232 - 10;
    RxFigures rx;
    rx.lastBadStatus = 0xFFFF;
    char line[512];

    // A day of streaming at 8000 packets a second, every packet in error.
    rx.packets = rx.dataCrc = rx.otherStatus = rx.lengthMismatch = 691'200'000ULL;
    rx.dbcGaps = rx.dbcBlocksMissed = rx.cyclesMissed = rx.cipChanges = 691'200'000ULL;
    EXPECT_LT(static_cast<size_t>(FormatRx(line, sizeof(line), ~0ULL, "stream", rx)), kRoom);

    ControllerFigures controller;
    controller.cycleLost = controller.cycleInconsistent = controller.cycleTooLong = 691'200'000ULL;
    controller.postedWriteErr = controller.regAccessFail = controller.unrecoverableError = 691'200'000ULL;
    controller.busResets = 100'000;
    controller.generation = 255;
    EXPECT_LT(static_cast<size_t>(FormatController(line, sizeof(line), "last 5.0 s", controller)), kRoom);

    AsyncNodeFigures node;
    node.requests = node.ackErr = node.busy = node.noAck = node.timeouts = node.otherEvent = 4'000'000'000U;
    EXPECT_LT(static_cast<size_t>(FormatAsync(line, sizeof(line), 63, "last 5.0 s", node)), kRoom);
}

TEST(LinkReport, TransmitControllerAndAsyncLines) {
    TxFigures tx;
    tx.packets = 40000;
    char line[224];
    FormatTx(line, sizeof(line), 0x0001F2000001F968ULL, "last 5.0 s", tx);
    EXPECT_STREQ(line, "[Link] 0x0001f2000001f968 last 5.0 s: tx 40000 pk, status err 0, cycles missed 0");
    EXPECT_FALSE(tx.AnyError());
    tx.cyclesMissed = 1;
    EXPECT_TRUE(tx.AnyError());

    ControllerFigures controller;
    controller.generation = 3;
    EXPECT_FALSE(controller.AnyError());
    controller.busResets = 1;
    EXPECT_TRUE(controller.AnyError());
    FormatController(line, sizeof(line), "last 5.0 s", controller);
    EXPECT_STREQ(line, "[Link] controller last 5.0 s: cycleLost 0, cycleInconsistent 0, cycleTooLong 0, "
                       "postedWriteErr 0, regAccessFail 0, unrecoverableError 0; bus resets 1 (generation 3)");

    AsyncNodeFigures node;
    node.requests = 12;
    node.busy = 2;
    EXPECT_TRUE(node.AnyTraffic());
    // Busy is the node's answer, not a link error.
    EXPECT_FALSE(node.AnyError());
    node.ackErr = 1;
    EXPECT_TRUE(node.AnyError());
    FormatAsync(line, sizeof(line), 1, "last 5.0 s", node);
    EXPECT_STREQ(line, "[Link] async to node 1 last 5.0 s: 12 requests, ack err 1, busy 2, no ack 0, "
                       "timeouts 0, other 0");
}

TEST(LinkReport, SpanIsTheIntervalInTenthsOfASecond) {
    char span[24];
    FormatSpan(span, sizeof(span), 5'000'000'000ULL);
    EXPECT_STREQ(span, "last 5.0 s");
    FormatSpan(span, sizeof(span), 5'098'000'000ULL);
    EXPECT_STREQ(span, "last 5.1 s");
}

TEST(LinkReport, NodeLineNamesRoleSpeedAndPorts) {
    const uint8_t ports[3] = {2, 0, 1};
    char line[224];
    FormatNode(line, sizeof(line), 7, 0, false, false, 400, true, 63, ports, 3);
    EXPECT_STREQ(line, "[Link] bus generation 7: node 0 S400, link on, gap count 63, ports 0:parent 2:idle");

    const uint8_t rootPorts[3] = {3, 3, 0};
    FormatNode(line, sizeof(line), 7, 2, true, true, 800, true, 63, rootPorts, 3);
    EXPECT_STREQ(line, "[Link] bus generation 7: node 2 (this controller, root) S800, link on, gap count 63, "
                       "ports 0:child 1:child");

    FormatNode(line, sizeof(line), 7, 1, false, false, 100, false, 5, nullptr, 0);
    EXPECT_STREQ(line, "[Link] bus generation 7: node 1 S100, link off, gap count 5, ports none");
}

TEST(AsyncLinkCounters, CountsByNodeAndByWhatTheCompletionSaid) {
    ASFW::Async::AsyncLinkCounters counters;
    counters.CountCompletion(0xFFC1, 0x11); // ack_complete
    counters.CountCompletion(0xFFC1, 0x12); // ack_pending
    counters.CountCompletion(0xFFC1, 0x1D); // ack_data_error
    counters.CountCompletion(0xFFC1, 0x14); // ack_busy_X
    counters.CountCompletion(0xFFC1, 0x03); // evt_missing_ack
    counters.CountCompletion(0xFFC1, 0x0A); // evt_timeout
    counters.CountCompletion(0xFFC1, 0x02); // evt_long_packet
    counters.CountCompletion(0xFFC2, 0x11);
    counters.CountSplitTimeout(0xFFC2);

    const auto one = AsyncNodeFigures::From(counters.node[1]);
    EXPECT_EQ(one.requests, 7U);
    EXPECT_EQ(one.ackErr, 1U);
    EXPECT_EQ(one.busy, 1U);
    EXPECT_EQ(one.noAck, 1U);
    EXPECT_EQ(one.timeouts, 1U);
    EXPECT_EQ(one.otherEvent, 1U);

    const auto two = AsyncNodeFigures::From(counters.node[2]);
    EXPECT_EQ(two.requests, 1U);
    EXPECT_EQ(two.timeouts, 1U);

    const auto since = two.Since(one);
    EXPECT_EQ(since.requests, 1U); // lower than before: counted from zero again
}

TEST(LinkStatsWire, LaysOutHeaderControllerUnitsNodesAndPorts) {
    StatsWire wire;
    ControllerFigures controller;
    controller.cycleLost = 3;
    controller.busResets = 2;
    controller.generation = 5;
    wire.SetController(controller);

    RxFigures rx;
    rx.packets = 480000;
    rx.dataCrc = 1;
    rx.speedCode = 2;
    TxFigures tx;
    tx.packets = 470000;
    ASSERT_TRUE(wire.AddUnit(0x0001F2000001F968ULL, true, false, rx, tx));

    AsyncNodeFigures node;
    node.requests = 40;
    node.busy = 6;
    ASSERT_TRUE(wire.AddAsyncNode(1, node));
    ASSERT_TRUE(wire.AddPhyPort(0, 0x0E, 0x40, 0, 0));
    // A unit after a node would land among the nodes.
    EXPECT_FALSE(wire.AddUnit(1, true, true, rx, tx));

    const uint64_t* w = wire.Words();
    EXPECT_EQ(w[0], StatsWire::kVersion);
    EXPECT_EQ(w[1], 1U);
    EXPECT_EQ(w[2], 1U);
    EXPECT_EQ(w[3], 1U);
    EXPECT_EQ(w[4], StatsWire::kPhyRead);
    EXPECT_EQ(w[5], 3U);
    EXPECT_EQ(w[11], 2U);
    EXPECT_EQ(w[12], 5U);
    const uint64_t* unit = w + 13;
    EXPECT_EQ(unit[0], 0x0001F2000001F968ULL);
    EXPECT_EQ(unit[1], StatsWire::kUnitRxRuns);
    EXPECT_EQ(unit[2], 2U);
    EXPECT_EQ(unit[3], 480000U);
    EXPECT_EQ(unit[4], 1U);
    EXPECT_EQ(unit[12], 470000U);
    const uint64_t* async = unit + StatsWire::kUnitWords;
    EXPECT_EQ(async[0], 1U);
    EXPECT_EQ(async[1], 40U);
    EXPECT_EQ(async[3], 6U);
    EXPECT_EQ(async[StatsWire::kAsyncWords], 0x400E00ULL);
    EXPECT_EQ(wire.WordCount(), 13U + StatsWire::kUnitWords + StatsWire::kAsyncWords + 1U);
    EXPECT_LE(StatsWire::kMaxWords * sizeof(uint64_t), 4096U);
}

} // namespace
