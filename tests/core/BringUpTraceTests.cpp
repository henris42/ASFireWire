// Unit tests for the numbered start steps (Logging/BringUpTrace.*): numbering
// per start, the active window for recurring paths, End, and long lines.

#include "Logging/BringUpTrace.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using ASFW::Logging::BringUpTrace;

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

} // namespace
