// Unit tests for the numbered start steps (Logging/BringUpTrace.*): numbering
// per start, the active window for recurring paths, End, long lines, and the
// lockstep mode: every step kept and published with a running total, detail
// steps only there, the newest lines kept across a full ring and a new start.

#include "Logging/BringUpTrace.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using ASFW::Logging::BringUpTrace;

struct Published {
    std::vector<std::string> lines;
    uint64_t total{0};
    uint32_t calls{0};
};

// Kept lines start with the step's uptime, "@<µs> "; the tests compare the rest.
std::string WithoutTime(const char* line) {
    std::string text(line);
    if (!text.empty() && text[0] == '@') {
        const auto space = text.find(' ');
        if (space != std::string::npos) text.erase(0, space + 1);
    }
    return text;
}

void Publish(void* context, const char* const* lines, uint32_t count, uint64_t total) {
    auto* published = static_cast<Published*>(context);
    published->lines.clear();
    for (uint32_t index = 0; index < count; ++index) {
        EXPECT_EQ(lines[index][0], '@') << "kept lines carry their own time";
        published->lines.push_back(WithoutTime(lines[index]));
    }
    published->total = total;
    ++published->calls;
}

TEST(BringUpTraceTests, StepsAreNumbered) {
    BringUpTrace trace;
    trace.Begin();
    trace.Step("pci %04x:%04x open", 0x104c, 0x823f);
    trace.Step("DMA %zu bytes at 0x%llx", size_t{1024}, 0x67a69000ull);

    ASSERT_EQ(trace.linesForTesting.size(), 2u);
    EXPECT_EQ(trace.linesForTesting[0], "[StartStep] #01 pci 104c:823f open");
    EXPECT_EQ(trace.linesForTesting[1], "[StartStep] #02 DMA 1024 bytes at 0x67a69000");
    EXPECT_EQ(trace.StepCount(), 2u);
}

TEST(BringUpTraceTests, ANewStartCountsFromOneAgain) {
    BringUpTrace trace;
    trace.Begin();
    trace.Step("first start");
    trace.Step("first start, step 2");
    trace.End("bus up");
    trace.Begin();
    trace.Step("second start");
    EXPECT_EQ(trace.linesForTesting.back(), "[StartStep] #01 second start");
    EXPECT_TRUE(trace.Active());
}

TEST(BringUpTraceTests, EndLogsItsReasonOnceAndClosesTheWindow) {
    BringUpTrace trace;
    EXPECT_FALSE(trace.Active());
    trace.Begin();
    EXPECT_TRUE(trace.Active());
    trace.Step("start");
    trace.End("bus up");
    trace.End("again");

    ASSERT_EQ(trace.linesForTesting.size(), 2u);
    EXPECT_EQ(trace.linesForTesting[1], "[StartStep] #02 bring-up ends: bus up");
    EXPECT_FALSE(trace.Active());
}

TEST(BringUpTraceTests, ALongLineIsCutRatherThanOverrun) {
    BringUpTrace trace;
    trace.Begin();
    const std::string longText(400, 'x');
    trace.Step("%s", longText.c_str());
    ASSERT_EQ(trace.linesForTesting.size(), 1u);
    EXPECT_LT(trace.linesForTesting[0].size(), 192u);
}

// Without lockstep a detail step costs nothing and leaves no line; with it,
// every step and detail is kept and published with the total so far.
TEST(BringUpTraceTests, DetailStepsExistOnlyInLockstep) {
    BringUpTrace trace;
    trace.Begin();
    trace.Detail("reg 0x%03x <- 0x%08x", 0x50u, 0x80000000u);
    EXPECT_TRUE(trace.linesForTesting.empty());
    EXPECT_EQ(trace.StepCount(), 0u);

    Published published;
    trace.SetLockstep(0, &Publish, &published);
    trace.Step("pci open");
    trace.Detail("reg 0x%03x <- 0x%08x", 0x50u, 0x80000000u);
    ASSERT_EQ(published.calls, 2u);
    ASSERT_EQ(published.lines.size(), 2u);
    EXPECT_EQ(published.lines[0], "[StartStep] #01 pci open");
    EXPECT_EQ(published.lines[1], "[StartStep] #02 reg 0x050 <- 0x80000000");
    EXPECT_EQ(published.total, 2u);
    EXPECT_EQ(trace.KeptTotal(), 2u);
}

// Detail steps belong to the bring-up window: none after End.
TEST(BringUpTraceTests, DetailStopsWithTheWindow) {
    BringUpTrace trace;
    Published published;
    trace.SetLockstep(0, &Publish, &published);
    trace.Begin();
    trace.End("bus up");
    trace.Detail("later register write");
    EXPECT_EQ(published.total, 1u) << "only the End step";
}

// The kept lines are the newest kKeptLines; the total keeps counting, so a
// reader that counts what it has written finds the new lines at the end.
TEST(BringUpTraceTests, AFullRingKeepsTheNewestLinesAndCountsOn) {
    BringUpTrace trace;
    Published published;
    trace.SetLockstep(0, &Publish, &published);
    trace.Begin();
    for (uint32_t index = 0; index < BringUpTrace::kKeptLines + 3; ++index) {
        trace.Step("step %u", index);
    }
    EXPECT_EQ(published.total, BringUpTrace::kKeptLines + 3);
    ASSERT_EQ(published.lines.size(), BringUpTrace::kKeptLines);
    EXPECT_EQ(published.lines.front(), "[StartStep] #04 step 3");
    EXPECT_EQ(published.lines.back(), "[StartStep] #259 step 258");
}

// A new start numbers its steps from one again but keeps the earlier lines,
// so one file shows every start of the driver's life.
TEST(BringUpTraceTests, ANewStartKeepsTheEarlierLines) {
    BringUpTrace trace;
    Published published;
    trace.SetLockstep(0, &Publish, &published);
    trace.Begin();
    trace.Step("first start");
    trace.End("bus up");
    trace.Begin();
    trace.Step("second start");
    ASSERT_EQ(published.lines.size(), 3u);
    EXPECT_EQ(published.lines[0], "[StartStep] #01 first start");
    EXPECT_EQ(published.lines[2], "[StartStep] #01 second start");
    EXPECT_EQ(published.total, 3u);
}

} // namespace
