// Logging stubs for host tests
// Provides no-op implementations to satisfy linker

#include "Logging/Logging.hpp"
#include <cstdarg>
#include <cstdio>

namespace ASFW::Driver::Logging {

// Stub log handles - just use default log for host tests
static os_log_t stub_log = OS_LOG_DEFAULT;

// DriverKit Stubs
extern "C" {
    typedef struct IOLock IOLock;
    
    IOLock* IOLockAlloc() { return (IOLock*)1; }
    void IOLockFree(IOLock* lock) {}
    void IOLockLock(IOLock* lock) {}
    void IOLockUnlock(IOLock* lock) {}
    
    void IOSleep(uint64_t milliseconds) {}
    
    void* IOMallocZero(size_t size) { return calloc(1, size); }
    void IOFree(void* ptr, size_t size) { free(ptr); }
}

os_log_t Core() {
    return stub_log;
}

os_log_t BusReset() {
    return stub_log;
}

os_log_t Topology() {
    return stub_log;
}

os_log_t ConfigROM() {
    return stub_log;
}

os_log_t Transaction() {
    return stub_log;
}

os_log_t Interrupt() {
    return stub_log;
}

os_log_t Controller() {
    return stub_log;
}

os_log_t Hardware() {
    return stub_log;
}

os_log_t Metrics() {
    return stub_log;
}

os_log_t Async() {
    return stub_log;
}

os_log_t UserClient() {
    return stub_log;
}

os_log_t Discovery() {
    return stub_log;
}

os_log_t IRM() {
    return stub_log;
}

os_log_t BusManager() {
    return stub_log;
}

os_log_t MusicSubunit() {
    return stub_log;
}

os_log_t FCP() {
    return stub_log;
}

os_log_t CMP() {
    return stub_log;
}

os_log_t AVC() {
    return stub_log;
}

os_log_t Isoch() {
    return stub_log;
}

os_log_t Audio() {
    return stub_log;
}

os_log_t DICE() {
    return stub_log;
}

os_log_t Oxfw() {
    return stub_log;
}

} // namespace ASFW::Driver::Logging
