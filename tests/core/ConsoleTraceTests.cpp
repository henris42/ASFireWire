// Unit tests for the bring-up console trace (Logging/ConsoleTrace.*): step
// numbering, the console limit, Finish and Unbind. On the host the console
// line lands in consoleLinesForTesting where DriverKit makes the kernel call.

#include "Logging/ConsoleTrace.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using ASFW::Logging::ConsoleTrace;

struct BoundTrace {
    IOService* service = new IOService();
    IODispatchQueue* queue = new IODispatchQueue();
    ConsoleTrace trace;

    BoundTrace() { trace.Bind(service, queue); }
    ~BoundTrace() {
        trace.Unbind();
        queue->release();
        service->release();
    }
};

TEST(ConsoleTraceTests, AnUnboundTraceCountsStepsButPrintsNothing) {
    ConsoleTrace trace;
    trace.Step("soft reset");
    trace.Step("link power status");
    EXPECT_EQ(trace.StepCount(), 2u);
    EXPECT_FALSE(trace.ConsoleActive());
    EXPECT_TRUE(trace.consoleLinesForTesting.empty());
}

TEST(ConsoleTraceTests, StepsAreNumberedOnTheConsole) {
    BoundTrace bound;
    bound.trace.Step("pci %04x:%04x open", 0x104c, 0x823f);
    bound.trace.Step("DMA %zu bytes at 0x%llx", size_t{1024}, 0x67a69000ull);

    ASSERT_EQ(bound.trace.consoleLinesForTesting.size(), 2u);
    EXPECT_EQ(bound.trace.consoleLinesForTesting[0], "ASFW #01 pci 104c:823f open");
    EXPECT_EQ(bound.trace.consoleLinesForTesting[1], "ASFW #02 DMA 1024 bytes at 0x67a69000");
}

TEST(ConsoleTraceTests, TheConsoleStopsAtTheLimitWhileStepsGoOn) {
    BoundTrace bound;
    for (uint32_t step = 0; step < ConsoleTrace::kConsoleStepLimit + 5; ++step) {
        bound.trace.Step("step");
    }
    EXPECT_EQ(bound.trace.consoleLinesForTesting.size(), ConsoleTrace::kConsoleStepLimit);
    EXPECT_EQ(bound.trace.StepCount(), ConsoleTrace::kConsoleStepLimit + 5);
}

TEST(ConsoleTraceTests, FinishPrintsItsReasonAndEndsTheConsole) {
    BoundTrace bound;
    bound.trace.Step("start");
    bound.trace.Finish("bus up");
    bound.trace.Step("after");
    bound.trace.Finish("again");

    ASSERT_EQ(bound.trace.consoleLinesForTesting.size(), 2u);
    EXPECT_EQ(bound.trace.consoleLinesForTesting[1], "ASFW #02 console trace ends: bus up");
    EXPECT_FALSE(bound.trace.ConsoleActive());
    EXPECT_EQ(bound.trace.StepCount(), 3u);
}

TEST(ConsoleTraceTests, UnbindEndsTheConsole) {
    BoundTrace bound;
    bound.trace.Step("start");
    bound.trace.Unbind();
    bound.trace.Step("stop");
    EXPECT_EQ(bound.trace.consoleLinesForTesting.size(), 1u);
}

TEST(ConsoleTraceTests, ALongLineIsCutRatherThanOverrun) {
    BoundTrace bound;
    const std::string longText(400, 'x');
    bound.trace.Step("%s", longText.c_str());
    ASSERT_EQ(bound.trace.consoleLinesForTesting.size(), 1u);
    // The kernel copies at most 255 characters of a queue name.
    EXPECT_LT(bound.trace.consoleLinesForTesting[0].size(), 255u);
}

} // namespace
