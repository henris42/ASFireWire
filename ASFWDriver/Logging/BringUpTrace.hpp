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
// Lockstep (the asfw-boot-trace property, b193 B760 report): a start that
// hangs the machine leaves nothing in the unified log, which logd buffers. In
// lockstep every step is also kept here and handed to a publisher (the driver
// puts the lines into its own I/O Registry entry), and the step then pauses
// for a set time, so a daemon polling the registry can write the line to disk
// and fsync it before the next step touches anything. Detail steps (each
// register write with its read-back, each kernel call, each poll) exist only
// in lockstep. After a hang the file's last line is the last thing that
// completed; the next step in the code is the one that hung.
//

#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>

#ifdef ASFW_HOST_TEST
#include <string>
#include <vector>
#endif

namespace ASFW::Logging {

class BringUpTrace {
public:
    // A kept line: "[StartStep] #NN " and the text (160 bytes at most).
    static constexpr uint32_t kLineBytes = 192;
    // Lines kept for the publisher: the newest kKeptLines, oldest first.
    static constexpr uint32_t kKeptLines = 256;
    // The publisher gets every kept line, oldest first, and the number of
    // lines there have been in all (so a reader can count what is new).
    using Publisher = void (*)(void* context, const char* const* lines, uint32_t count,
                               uint64_t total);

    BringUpTrace() = default;
    BringUpTrace(const BringUpTrace&) = delete;
    BringUpTrace& operator=(const BringUpTrace&) = delete;

    // A new start: steps count from #01 again and the trace is active.
    void Begin() noexcept;

    // Logs "[StartStep] #NN <text>".
    void Step(const char* format, ...) noexcept __attribute__((format(printf, 2, 3)));

    // A step only in lockstep while the trace is active: the detail between
    // the milestones. Costs a load otherwise.
    void Detail(const char* format, ...) noexcept __attribute__((format(printf, 2, 3)));

    // Bring-up is over; recurring paths stop adding steps.
    void End(const char* reason) noexcept;

    // Lockstep on: keep and publish every step, then pause `pauseMs`. The
    // kept lines survive Begin(), so a reader sees every start of the
    // driver's life; `total` keeps counting across them.
    void SetLockstep(uint32_t pauseMs, Publisher publish, void* context) noexcept;

    [[nodiscard]] bool Active() const noexcept { return active_.load(std::memory_order_acquire); }
    [[nodiscard]] bool Lockstep() const noexcept { return publish_ != nullptr; }
    [[nodiscard]] uint32_t StepCount() const noexcept {
        return sequence_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t KeptTotal() const noexcept { return keptTotal_; }

#ifdef ASFW_HOST_TEST
    std::vector<std::string> linesForTesting;
#endif

private:
    void Emit(const char* format, va_list args) noexcept;
    void Keep(const char* line) noexcept;

    std::atomic<uint32_t> sequence_{0};
    std::atomic<bool> active_{false};

    uint32_t pauseMs_{0};
    Publisher publish_{nullptr};
    void* publishContext_{nullptr};
    // The start thread and the interrupt thread both step during bring-up.
    std::atomic_flag keptLock_ = ATOMIC_FLAG_INIT;
    uint64_t keptTotal_{0};
    char kept_[kKeptLines][kLineBytes]{};
    const char* keptOrder_[kKeptLines]{};
};

} // namespace ASFW::Logging
