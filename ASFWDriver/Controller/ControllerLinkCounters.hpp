// ControllerLinkCounters.hpp
// ASFW - bus and controller events the link raised, counted as they are handled.

#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Driver {

// Written on the interrupt queue, read from anywhere. Since driver start.
struct ControllerLinkCounters final {
    // No cycle start arrived for a cycle (this node is not cycle master) or
    // none went out. The bit is masked outside the root probe, so this counts
    // interrupt passes that found it set: at least that many cycles.
    std::atomic<uint64_t> cycleLost{0};
    // A cycle start arrived whose time disagreed with the local cycle timer
    // (outside a bus reset, where it is expected).
    std::atomic<uint64_t> cycleInconsistent{0};
    // Isochronous transmission ran past the cycle; the controller dropped
    // cycle master and the driver set it again.
    std::atomic<uint64_t> cycleTooLong{0};
    std::atomic<uint64_t> postedWriteErr{0};
    std::atomic<uint64_t> regAccessFail{0};
    std::atomic<uint64_t> unrecoverableError{0};
    std::atomic<uint64_t> busResets{0};
    std::atomic<uint32_t> generation{0};

    static void Add(std::atomic<uint64_t>& counter) noexcept {
        counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
};

} // namespace ASFW::Driver
