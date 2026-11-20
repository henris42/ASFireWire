// Unit tests for the [Glitch] event log (Audio/DriverKit/Runtime/GlitchEventLog.hpp):
// events come out oldest first with their 64-bit sample position, a writer
// that laps the reader overwrites and the reader counts the loss, a slot
// still being written ends a take, and a stream start drops what is left.
// And the IO operation trace: a snapshot of the newest operations, oldest
// first, without consuming them; one capture at a time for the report.

#include <gtest/gtest.h>

#include "Audio/DriverKit/Runtime/GlitchEventLog.hpp"
#include "Audio/DriverKit/Runtime/IoOpTiming.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using ASFW::Audio::Runtime::GlitchEvent;
using ASFW::Audio::Runtime::GlitchEventKind;
using ASFW::Audio::Runtime::GlitchEventLog;
using ASFW::Audio::Runtime::IoOp;
using ASFW::Audio::Runtime::IoOpTrace;
using ASFW::Audio::Runtime::IoOpFollows;
using ASFW::Audio::Runtime::IoOpFollowTag;
using ASFW::Audio::Runtime::IoOpTiming;
using ASFW::Audio::Runtime::IoOpTraceCapture;
using ASFW::Audio::Runtime::IoOffsetSpan;
using ASFW::Audio::Runtime::IoRenderPhaseSpan;
using ASFW::Audio::Runtime::IoStepSpan;
using ASFW::Audio::Runtime::IoStepTiming;

GlitchEvent ShortRead(uint64_t frame) {
    GlitchEvent event{};
    event.sampleFrame = frame;
    event.kind = GlitchEventKind::ShortCaptureRead;
    event.frames = 64;
    return event;
}

TEST(GlitchEventLogTests, EventsComeOutOldestFirstWithTheirSamplePosition) {
    GlitchEventLog log;
    const uint64_t beyond32Bits = (uint64_t{1} << 33) + 7;  // a 64-bit position survives
    log.Record(ShortRead(100));
    log.Record(ShortRead(beyond32Bits));
    std::array<GlitchEvent, 8> out{};
    uint64_t lost = 0;
    ASSERT_EQ(log.Take(out.data(), out.size(), lost), 2U);
    EXPECT_EQ(out[0].sampleFrame, 100U);
    EXPECT_EQ(out[1].sampleFrame, beyond32Bits);
    EXPECT_EQ(lost, 0U);
    EXPECT_EQ(log.Pending(), 0U);
}

TEST(GlitchEventLogTests, ATakeStopsAtItsLimitAndTheRestWaits) {
    GlitchEventLog log;
    for (uint64_t frame = 0; frame < 10; ++frame) log.Record(ShortRead(frame));
    std::array<GlitchEvent, 4> out{};
    uint64_t lost = 0;
    ASSERT_EQ(log.Take(out.data(), out.size(), lost), 4U);
    EXPECT_EQ(out[3].sampleFrame, 3U);
    EXPECT_EQ(log.Pending(), 6U);
    ASSERT_EQ(log.Take(out.data(), out.size(), lost), 4U);
    EXPECT_EQ(out[0].sampleFrame, 4U);
}

TEST(GlitchEventLogTests, AWriterThatLapsTheReaderOverwritesAndTheLossIsCounted) {
    GlitchEventLog log;
    const uint64_t recorded = GlitchEventLog::kCapacity + 10;
    for (uint64_t frame = 0; frame < recorded; ++frame) log.Record(ShortRead(frame));
    std::vector<GlitchEvent> out(GlitchEventLog::kCapacity);
    uint64_t lost = 0;
    ASSERT_EQ(log.Take(out.data(), static_cast<uint32_t>(out.size()), lost), GlitchEventLog::kCapacity);
    EXPECT_EQ(lost, 10U);
    EXPECT_EQ(out.front().sampleFrame, 10U) << "the oldest kept event";
    EXPECT_EQ(out.back().sampleFrame, recorded - 1);
}

TEST(GlitchEventLogTests, AStreamStartDropsWhatIsLeft) {
    GlitchEventLog log;
    log.Record(ShortRead(1));
    log.Reset();
    std::array<GlitchEvent, 2> out{};
    uint64_t lost = 0;
    EXPECT_EQ(log.Take(out.data(), out.size(), lost), 0U);
    log.Record(ShortRead(2));
    ASSERT_EQ(log.Take(out.data(), out.size(), lost), 1U);
    EXPECT_EQ(out[0].sampleFrame, 2U);
}

// Two IO threads (one per client connection) write at once; every event the
// reader returns is whole, and with room to spare none is lost.
TEST(GlitchEventLogTests, TwoWritersAtOnceLoseNothingWithRoomToSpare) {
    GlitchEventLog log;
    constexpr uint64_t kPerWriter = GlitchEventLog::kCapacity / 2;
    auto writer = [&log](uint64_t base) {
        for (uint64_t i = 0; i < kPerWriter; ++i) {
            GlitchEvent event = ShortRead(base + i);
            event.missing = static_cast<uint16_t>(base + i);  // checked against the frame
            log.Record(event);
        }
    };
    std::thread a(writer, 0);
    std::thread b(writer, 1000);
    a.join();
    b.join();
    std::vector<GlitchEvent> out(GlitchEventLog::kCapacity);
    uint64_t lost = 0;
    const uint32_t taken = log.Take(out.data(), static_cast<uint32_t>(out.size()), lost);
    EXPECT_EQ(taken, 2 * kPerWriter);
    EXPECT_EQ(lost, 0U);
    for (uint32_t i = 0; i < taken; ++i) {
        EXPECT_EQ(out[i].missing, static_cast<uint16_t>(out[i].sampleFrame)) << "a torn event";
    }
}

IoOp Op(uint64_t entry, uint16_t operation) {
    IoOp op{};
    op.entryTicks = entry;
    op.sampleTime = entry * 10;
    op.operation = operation;
    return op;
}

// A snapshot gives the newest operations oldest first and leaves them in
// place; a trace with fewer operations than asked gives what it has.
TEST(IoOpTraceTests, ASnapshotGivesTheNewestOperationsOldestFirstWithoutTakingThem) {
    IoOpTrace trace;
    std::array<IoOp, IoOpTrace::kCapacity> out{};
    EXPECT_EQ(trace.Snapshot(out.data(), out.size()), 0U);
    for (uint64_t entry = 1; entry <= IoOpTrace::kCapacity + 5; ++entry) {
        trace.Record(Op(entry, static_cast<uint16_t>(entry % 2)));
    }
    ASSERT_EQ(trace.Snapshot(out.data(), 4), 4U);
    EXPECT_EQ(out[0].entryTicks, IoOpTrace::kCapacity + 2);
    EXPECT_EQ(out[3].entryTicks, IoOpTrace::kCapacity + 5);
    EXPECT_EQ(out[3].sampleTime, (IoOpTrace::kCapacity + 5) * 10);
    ASSERT_EQ(trace.Snapshot(out.data(), out.size()), IoOpTrace::kCapacity) << "a lap keeps the newest ring";
    EXPECT_EQ(out[0].entryTicks, 6U);
    EXPECT_EQ(trace.Recorded(), IoOpTrace::kCapacity + 5);
    EXPECT_EQ(trace.Snapshot(out.data(), out.size()), IoOpTrace::kCapacity) << "nothing was consumed";
}

// Two IO threads record at once; every operation a snapshot returns is whole.
TEST(IoOpTraceTests, TwoWritersAtOnceGiveWholeOperations) {
    IoOpTrace trace;
    auto writer = [&trace](uint64_t base) {
        for (uint64_t i = 0; i < 200; ++i) {
            trace.Record(Op(base + i, 0));
        }
    };
    std::thread a(writer, 1);
    std::thread b(writer, 100'000);
    a.join();
    b.join();
    std::array<IoOp, IoOpTrace::kCapacity> out{};
    const uint32_t count = trace.Snapshot(out.data(), out.size());
    EXPECT_LE(count, IoOpTrace::kCapacity);
    for (uint32_t i = 0; i < count; ++i) {
        EXPECT_EQ(out[i].sampleTime, out[i].entryTicks * 10) << "a torn operation";
    }
}

// A capture holds the operations before the read ahead and the read itself
// last; a second read while one capture waits is not captured; the reader
// frees the slot, and a stream start drops a capture nobody took.
TEST(IoOpTraceTests, OneCaptureWaitsForTheReportWithTheReadAheadLast) {
    IoOpTrace trace;
    IoOpTraceCapture capture;
    for (uint64_t entry = 1; entry <= 3; ++entry) {
        trace.Record(Op(entry, 0));
    }
    const IoOp ahead = Op(4, 0);
    EXPECT_TRUE(capture.TryCapture(trace, ahead));
    trace.Record(ahead);
    EXPECT_FALSE(capture.TryCapture(trace, Op(5, 0))) << "a capture already waits";
    std::array<IoOp, IoOpTraceCapture::kMaxOps> out{};
    ASSERT_EQ(capture.Take(out.data(), out.size()), 4U);
    EXPECT_EQ(out[0].entryTicks, 1U);
    EXPECT_EQ(out[3].entryTicks, 4U) << "the read ahead comes last";
    EXPECT_EQ(capture.Take(out.data(), out.size()), 0U) << "taken once";
    EXPECT_TRUE(capture.TryCapture(trace, Op(6, 0)));
    capture.Reset();
    EXPECT_EQ(capture.Take(out.data(), out.size()), 0U);
}

// IoOpTiming: a step counts every operation; a report takes what came since
// the last one, with that interval's longest, and the stream keeps its own.
TEST(IoOpTimingTests, AReportTakesItsIntervalAndTheStreamKeepsItsTotals) {
    IoStepTiming step;
    step.Record(10);
    step.Record(30);
    IoStepSpan span = step.Take();
    EXPECT_EQ(span.count, 2U);
    EXPECT_EQ(span.ticks, 40U);
    EXPECT_EQ(span.maxTicks, 30U);
    step.Record(5);
    span = step.Take();
    EXPECT_EQ(span.count, 1U);
    EXPECT_EQ(span.ticks, 5U);
    EXPECT_EQ(span.maxTicks, 5U) << "the interval's longest, not the stream's";
    span = step.Take();
    EXPECT_EQ(span.count, 0U);
    EXPECT_EQ(span.maxTicks, 0U);
    const IoStepSpan stream = step.Stream();
    EXPECT_EQ(stream.count, 3U);
    EXPECT_EQ(stream.ticks, 45U);
    EXPECT_EQ(stream.maxTicks, 30U);
    step.Reset();
    EXPECT_EQ(step.Stream().count, 0U);
    EXPECT_EQ(step.Take().count, 0U);
}

// Before a stream starts no operation is slow; after, the threshold is the
// one the start gave, and a slow operation is kept whole.
TEST(IoOpTimingTests, SlowOperationsAreCountedAndKept) {
    IoOpTiming timing;
    EXPECT_EQ(timing.slowTicks, UINT64_MAX);
    timing.Reset(/*nowTicks=*/100, /*slowThresholdTicks=*/50, /*followLimitTicks=*/200,
                 /*pullWaits=*/7, /*pullBusy=*/3);
    EXPECT_EQ(timing.slowTicks, 50U);
    EXPECT_EQ(timing.streamStartTicks, 100U);
    EXPECT_EQ(timing.reportedPullWaits, 7U);
    EXPECT_EQ(timing.streamStartPullBusy, 3U);
    IoOp op = Op(1234, 0);
    op.durationNanos = 61'000;
    op.partNanos = 55'000;
    timing.RecordSlow(op);
    EXPECT_EQ(timing.slow.load(), 1U);
    std::array<IoOp, 4> out{};
    ASSERT_EQ(timing.slowOps.Snapshot(out.data(), out.size()), 1U);
    EXPECT_EQ(out[0].entryTicks, 1234U);
    EXPECT_EQ(out[0].partNanos, 55'000U);
    // The next stream starts from nothing; the operations kept from the last
    // one are behind what a report has covered.
    timing.Reset(200, 50, 200, 7, 3);
    EXPECT_EQ(timing.slow.load(), 0U);
    EXPECT_EQ(timing.reportedSlowOps, timing.slowOps.Recorded());
}

// "Follow": only the same operation on another device counts, and only a gap
// short enough to be two operations back to back.
TEST(IoOpTimingTests, AnOperationFollowsTheSameOperationOnAnotherDevice) {
    alignas(8) uint64_t deviceA = 0;
    alignas(8) uint64_t deviceB = 0;
    const uintptr_t readA = IoOpFollowTag(&deviceA, /*writeEnd=*/false);
    const uintptr_t writeA = IoOpFollowTag(&deviceA, /*writeEnd=*/true);
    EXPECT_TRUE(IoOpFollows(readA, &deviceB, false));
    EXPECT_TRUE(IoOpFollows(writeA, &deviceB, true));
    EXPECT_FALSE(IoOpFollows(readA, &deviceB, true)) << "a WriteEnd after a BeginRead is the client's time";
    EXPECT_FALSE(IoOpFollows(readA, &deviceA, false)) << "the same device: the next cycle, or a second client";
    EXPECT_FALSE(IoOpFollows(0, &deviceB, false)) << "nothing returned yet";

    IoOpTiming timing;
    timing.Reset(0, 50, /*followLimitTicks=*/200, 0, 0);
    timing.RecordFollow(40);
    timing.RecordFollow(25);
    timing.RecordFollow(200);  // the IO cycle itself
    const IoStepSpan follow = timing.follow.Stream();
    EXPECT_EQ(follow.count, 2U);
    EXPECT_EQ(follow.ticks, 65U);
    EXPECT_EQ(timing.followMinTicks.load(), 25U);
}

// When CoreAudio got to an operation: BeginRead after the host time of its
// first frame, WriteEnd before the host time of its first frame, signed (a
// write that arrives after its presentation time has a negative margin).
TEST(IoOpTimingTests, ArrivalsAreMeasuredAgainstCoreAudiosHostTime) {
    IoOpTiming timing;
    timing.Reset(0, 50, 200, 0, 0);
    timing.RecordArrival(/*isWriteEnd=*/false, 10'000, 8'600);
    timing.RecordArrival(false, 20'000, 18'500);
    timing.RecordArrival(false, 30'000, 0);  // no host time given
    timing.RecordArrival(/*isWriteEnd=*/true, 10'700, 12'000);
    timing.RecordArrival(true, 21'000, 20'800);  // late: 200 after its presentation
    IoOffsetSpan read = timing.readArrival.Take();
    EXPECT_EQ(read.count, 2U);
    EXPECT_EQ(read.ticks, 2'900);
    EXPECT_EQ(read.minTicks, 1'400);
    EXPECT_EQ(read.maxTicks, 1'500);
    IoOffsetSpan write = timing.writeMargin.Take();
    EXPECT_EQ(write.count, 2U);
    EXPECT_EQ(write.ticks, 1'100);
    EXPECT_EQ(write.minTicks, -200);
    EXPECT_EQ(write.maxTicks, 1'300);

    // The next interval has its own least and most; the stream keeps both.
    timing.RecordArrival(true, 30'000, 30'900);
    write = timing.writeMargin.Take();
    EXPECT_EQ(write.count, 1U);
    EXPECT_EQ(write.minTicks, 900);
    EXPECT_EQ(write.maxTicks, 900);
    EXPECT_EQ(timing.writeMargin.Take().count, 0U);
    const IoOffsetSpan stream = timing.writeMargin.Stream();
    EXPECT_EQ(stream.count, 3U);
    EXPECT_EQ(stream.ticks, 2'000);
    EXPECT_EQ(stream.minTicks, -200);
    EXPECT_EQ(stream.maxTicks, 1'300);
    timing.Reset(0, 50, 200, 0, 0);
    EXPECT_EQ(timing.writeMargin.Stream().count, 0U);
    EXPECT_EQ(timing.readArrival.Take().count, 0U);
}

// Render: from this device's BeginRead returning to its WriteEnd beginning.
// A WriteEnd with no BeginRead since the last one (an output-only client)
// has none.
TEST(IoOpTimingTests, RenderRunsFromBeginReadsReturnToWriteEndsEntry) {
    IoOpTiming timing;
    timing.Reset(0, 50, 200, 0, 0);
    timing.RecordWriteEntry(900, 64);  // no BeginRead before it
    timing.RecordReadReturn(1'000);
    timing.RecordWriteEntry(1'450, 64);
    timing.RecordWriteEntry(1'500, 64);  // a second WriteEnd of the same cycle
    timing.RecordReadReturn(2'000);
    timing.RecordReadReturn(3'000);  // a read without a write between
    timing.RecordWriteEntry(3'700, 64);
    const IoStepSpan render = timing.render.Stream();
    EXPECT_EQ(render.count, 2U);
    EXPECT_EQ(render.ticks, 1'150U);
    EXPECT_EQ(render.maxTicks, 700U);
    EXPECT_EQ(timing.writtenFrames.load(), 256U);
}

// Render by position in 8192 written frames: a client that is slow once in
// every 128 cycles of 64 frames shows as one position, in an interval's
// report and in the stream's.
TEST(IoOpTimingTests, APeriodicLongRenderShowsAtItsPosition) {
    IoOpTiming timing;
    timing.Reset(0, 50, 200, 0, 0);
    uint64_t now = 1'000;
    const auto cycles = [&](uint32_t count, uint32_t slowCycle) {
        for (uint32_t index = 0; index < count; ++index) {
            const uint64_t written = timing.writtenFrames.load();
            const bool slow = (written / 64) % 128 == slowCycle;
            timing.RecordReadReturn(now);
            now += slow ? 700 : 400 + (index % 3);
            timing.RecordWriteEntry(now, 64);
            now += 100;
        }
    };
    cycles(128 * 40, 37);
    IoRenderPhaseSpan span = timing.renderPhases.Span(/*take=*/true);
    EXPECT_EQ(span.positions, 128U);
    EXPECT_EQ(span.highFrame, 37U * 64U);
    EXPECT_EQ(span.highCount, 40U);
    EXPECT_EQ(span.highTicks, 40U * 700U);
    EXPECT_LE(span.lowTicks / span.lowCount, 402U);

    // The next interval's position is its own.
    cycles(128 * 10, 90);
    span = timing.renderPhases.Span(/*take=*/true);
    EXPECT_EQ(span.highFrame, 90U * 64U);
    EXPECT_EQ(span.highCount, 10U);
    // The stream has both; the earlier, longer one has as high a mean.
    span = timing.renderPhases.Span(/*take=*/false);
    EXPECT_EQ(span.positions, 128U);
    EXPECT_GE(span.highTicks / span.highCount, 400U + 300U * 10U / 50U);

    // A position only a few stray renders fell on is left out.
    timing.Reset(0, 50, 200, 0, 0);
    now = 1'000;
    cycles(128 * 40, 37);
    timing.writtenFrames.fetch_add(32);  // one odd-sized write
    timing.RecordReadReturn(now);
    timing.RecordWriteEntry(now + 5'000, 64);
    span = timing.renderPhases.Span(/*take=*/false);
    EXPECT_EQ(span.positions, 129U);
    EXPECT_EQ(span.highFrame, 37U * 64U);

    timing.Reset(0, 50, 200, 0, 0);
    EXPECT_EQ(timing.renderPhases.Span(false).positions, 0U);
    EXPECT_EQ(timing.render.Stream().count, 0U);
}

} // namespace
