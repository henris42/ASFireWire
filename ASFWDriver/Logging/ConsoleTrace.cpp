//
// ConsoleTrace.cpp
// ASFWDriver
//

#include "ConsoleTrace.hpp"

#include <cstdarg>
#include <cstdio>

#include "Logging.hpp"

namespace ASFW::Logging {

void ConsoleTrace::Bind(IOService* service, IODispatchQueue* queue) noexcept {
    queue_.store(queue, std::memory_order_release);
    service_.store(service, std::memory_order_release);
}

void ConsoleTrace::Unbind() noexcept {
    service_.store(nullptr, std::memory_order_release);
    queue_.store(nullptr, std::memory_order_release);
}

void ConsoleTrace::Step(const char* format, ...) noexcept {
    char text[160];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    const uint32_t step = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    ASFW_LOG(Controller, "[StartStep] #%02u %{public}s", step, text);

    if (step > kConsoleStepLimit || !ConsoleActive()) {
        return;
    }
    char line[192];
    (void)snprintf(line, sizeof(line), "ASFW #%02u %s", step, text);
    ToConsole(line);
}

void ConsoleTrace::Finish(const char* reason) noexcept {
    if (finished_.load(std::memory_order_acquire)) {
        return;
    }
    Step("console trace ends: %s", reason);
    finished_.store(true, std::memory_order_release);
}

void ConsoleTrace::ToConsole(const char* line) noexcept {
    IOService* service = service_.load(std::memory_order_acquire);
    IODispatchQueue* queue = queue_.load(std::memory_order_acquire);
    if (service == nullptr || queue == nullptr) {
        return;
    }
#ifdef ASFW_HOST_TEST
    consoleLinesForTesting.emplace_back(line);
#else
    // No queue is declared under this name, so the call fails with
    // kIOReturnBadArgument after the kernel has printed it (see the header).
    (void)service->SetDispatchQueue(line, queue);
#endif
}

} // namespace ASFW::Logging
