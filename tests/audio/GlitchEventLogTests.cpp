// Unit tests for the [Glitch] event log (Audio/DriverKit/Runtime/GlitchEventLog.hpp):
// events come out oldest first with their 64-bit sample position, a writer
// that laps the reader overwrites and the reader counts the loss, a slot
// still being written ends a take, and a stream start drops what is left.

#include <gtest/gtest.h>

#include "Audio/DriverKit/Runtime/GlitchEventLog.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using ASFW::Audio::Runtime::GlitchEvent;
using ASFW::Audio::Runtime::GlitchEventKind;
using ASFW::Audio::Runtime::GlitchEventLog;

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

} // namespace
