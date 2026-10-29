//
// BringUpTrace.hpp
// ASFWDriver
//
// Numbered bring-up steps, "[StartStep] #NN <text>": one line per stage of a
// driver start (PCI open, interrupts, DMA buffers, controller reset, link
// enable, first interrupts, self-IDs, first ROM scan), so a start that stalls
// or fails can be read step by step. They are milestones, logged at every
// level from warn up, and asfw-config-agent saves each boot's steps to
// ~/Library/Logs/ASFW.
//
// Steps from paths that recur after bring-up (interrupts, self-ID decode, DMA
// allocations) are added only while the trace is active: from Begin() at the
// start of Start() until End() after the first ROM scan.
//

#pragma once

#include <atomic>
#include <cstdint>

#ifdef ASFW_HOST_TEST
#include <string>
#include <vector>
#endif

namespace ASFW::Logging {

class BringUpTrace {
public:
    BringUpTrace() = default;
    BringUpTrace(const BringUpTrace&) = delete;
    BringUpTrace& operator=(const BringUpTrace&) = delete;

    // A new start: steps count from #01 again and the trace is active.
    void Begin() noexcept;

    // Logs "[StartStep] #NN <text>".
    void Step(const char* format, ...) noexcept __attribute__((format(printf, 2, 3)));

    // Bring-up is over; recurring paths stop adding steps.
    void End(const char* reason) noexcept;

    [[nodiscard]] bool Active() const noexcept { return active_.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t StepCount() const noexcept {
        return sequence_.load(std::memory_order_relaxed);
    }

#ifdef ASFW_HOST_TEST
    std::vector<std::string> linesForTesting;
#endif

private:
    std::atomic<uint32_t> sequence_{0};
    std::atomic<bool> active_{false};
};

} // namespace ASFW::Logging
