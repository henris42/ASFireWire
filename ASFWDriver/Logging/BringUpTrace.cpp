//
// BringUpTrace.cpp
// ASFWDriver
//

#include "BringUpTrace.hpp"

#include <cstdio>
#include <cstring>

#include "Logging.hpp"
#include "../Common/TimingUtils.hpp"

#include <DriverKit/IOLib.h>  // IOSleep, IODelay (the host tests' mock in tests/mocks)

namespace ASFW::Logging {

void BringUpTrace::Begin() noexcept {
    sequence_.store(0, std::memory_order_relaxed);
    active_.store(true, std::memory_order_release);
}

void BringUpTrace::SetLockstep(uint32_t pauseMs, Publisher publish, void* context) noexcept {
    pauseMs_ = pauseMs;
    publishContext_ = context;
    publish_ = publish;
}

void BringUpTrace::Step(const char* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    Emit(format, args);
    va_end(args);
}

void BringUpTrace::Detail(const char* format, ...) noexcept {
    if (!Lockstep() || !active_.load(std::memory_order_acquire)) {
        return;
    }
    va_list args;
    va_start(args, format);
    Emit(format, args);
    va_end(args);
}

void BringUpTrace::Emit(const char* format, va_list args) noexcept {
    const uint32_t step = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    // A milestone, logged from warn up; below that, skip the formatting too,
    // unless lockstep keeps the line.
    const bool log = ASFW_LOG_ENABLED(Controller, Warning);
    const bool keep = Lockstep();
    if (!log && !keep) {
        return;
    }
    char text[160];
    (void)vsnprintf(text, sizeof(text), format, args);

    if (log) {
        ASFW_LOG_MILESTONE(Controller, "[StartStep] #%02u %{public}s", step, text);
    }
    char line[kLineBytes];
    (void)snprintf(line, sizeof(line), "[StartStep] #%02u %s", step, text);
#ifdef ASFW_HOST_TEST
    linesForTesting.emplace_back(line);
#endif
    if (keep) {
        // The kept line carries the step's own time (host uptime, µs): the
        // daemon reads the registry late when the system is busy (b208: up to
        // a minute behind the kernel at boot) and converts this to wall time.
        char kept[kLineBytes];
        const uint64_t uptimeUs = ASFW::Timing::hostTicksToNanos(mach_absolute_time()) / 1000;
        (void)snprintf(kept, sizeof(kept), "@%llu %s", static_cast<unsigned long long>(uptimeUs), line);
        Keep(kept);
        if (pauseMs_ != 0) {
            IOSleep(pauseMs_);
        }
    }
}

void BringUpTrace::Keep(const char* line) noexcept {
    while (keptLock_.test_and_set(std::memory_order_acquire)) {
        IODelay(10);
    }
    (void)strlcpy(kept_[keptTotal_ % kKeptLines], line, kLineBytes);
    ++keptTotal_;
    const uint64_t count = keptTotal_ < kKeptLines ? keptTotal_ : kKeptLines;
    for (uint64_t index = 0; index < count; ++index) {
        keptOrder_[index] = kept_[(keptTotal_ - count + index) % kKeptLines];
    }
    publish_(publishContext_, keptOrder_, static_cast<uint32_t>(count), keptTotal_);
    keptLock_.clear(std::memory_order_release);
}

void BringUpTrace::End(const char* reason) noexcept {
    if (!active_.load(std::memory_order_acquire)) {
        return;
    }
    Step("bring-up ends: %s", reason);
    active_.store(false, std::memory_order_release);
}

} // namespace ASFW::Logging
