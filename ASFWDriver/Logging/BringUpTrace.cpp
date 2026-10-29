//
// BringUpTrace.cpp
// ASFWDriver
//

#include "BringUpTrace.hpp"

#include <cstdarg>
#include <cstdio>

#include "Logging.hpp"

namespace ASFW::Logging {

void BringUpTrace::Begin() noexcept {
    sequence_.store(0, std::memory_order_relaxed);
    active_.store(true, std::memory_order_release);
}

void BringUpTrace::Step(const char* format, ...) noexcept {
    const uint32_t step = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    // A milestone, logged from warn up; below that, skip the formatting too.
    if (!ASFW_LOG_ENABLED(Controller, Warning)) {
        return;
    }
    char text[160];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    ASFW_LOG_MILESTONE(Controller, "[StartStep] #%02u %{public}s", step, text);
#ifdef ASFW_HOST_TEST
    char line[192];
    (void)snprintf(line, sizeof(line), "[StartStep] #%02u %s", step, text);
    linesForTesting.emplace_back(line);
#endif
}

void BringUpTrace::End(const char* reason) noexcept {
    if (!active_.load(std::memory_order_acquire)) {
        return;
    }
    Step("bring-up ends: %s", reason);
    active_.store(false, std::memory_order_release);
}

} // namespace ASFW::Logging
