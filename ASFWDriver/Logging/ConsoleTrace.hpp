//
// ConsoleTrace.hpp
// ASFWDriver
//
// Numbered bring-up steps, written to os_log and to the kernel console.
//
// A hard freeze takes every line a dext has logged with it. os_log and IOLog
// from a dext reach only the kernel message buffer and logd (DriverKit's
// libsystem_trace hands them over with log_data_as_kernel); neither survives a
// freeze, and neither is printed on the console. The kernel does print, through
// its own IOLog and so on the console and a serial port, the name passed to
// every IOService::SetDispatchQueue call when the boot-args carry dk with bit
// 0x2 (xnu IOUserServer.cpp, IOService::SetDispatchQueue_Impl). A name the
// service declares no queue for fails with kIOReturnBadArgument and changes
// nothing, in the kernel or in DriverKit, whose SetDispatchQueue_Impl returns
// the kernel's error before it touches anything. So a step costs one kernel
// call, and with serial=... drain_uart_sync=1 its text is on the wire before
// the next step starts.
//
// dk bit 0x1 is what enables DriverKit at all (kIODKEnable, the default): use
// dk=0x3. dk=0x2 alone would launch no driver.
//

#pragma once

#include <atomic>
#include <cstdint>

#ifdef ASFW_HOST_TEST
#include <string>
#include <vector>
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOService.h>
#endif

namespace ASFW::Logging {

class ConsoleTrace {
public:
    // Steps after this many go to os_log only; a bring-up takes about 50.
    static constexpr uint32_t kConsoleStepLimit = 120;

    ConsoleTrace() = default;
    ConsoleTrace(const ConsoleTrace&) = delete;
    ConsoleTrace& operator=(const ConsoleTrace&) = delete;

    // service: this driver instance (the kernel prints its name and registry
    // ID before the text). queue: any of its queues; the kernel only records it.
    // Both must outlive the binding: Unbind before either is released.
    void Bind(IOService* service, IODispatchQueue* queue) noexcept;
    void Unbind() noexcept;

    // Logs "[StartStep] #NN <text>" and, while bound and within the limit,
    // prints "ASFW #NN <text>" on the kernel console.
    void Step(const char* format, ...) noexcept __attribute__((format(printf, 2, 3)));

    // Ends console output: the steps that follow are os_log only.
    void Finish(const char* reason) noexcept;

    [[nodiscard]] uint32_t StepCount() const noexcept {
        return sequence_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool ConsoleActive() const noexcept {
        return !finished_.load(std::memory_order_acquire) &&
               service_.load(std::memory_order_acquire) != nullptr;
    }

#ifdef ASFW_HOST_TEST
    std::vector<std::string> consoleLinesForTesting;
#endif

private:
    void ToConsole(const char* line) noexcept;

    std::atomic<IOService*> service_{nullptr};
    std::atomic<IODispatchQueue*> queue_{nullptr};
    std::atomic<uint32_t> sequence_{0};
    std::atomic<bool> finished_{false};
};

} // namespace ASFW::Logging
