// AsyncLinkCounters.hpp
// ASFW - how the requests sent to each node ended at the link.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Async {

// Per destination node (physical ID, 0..63), since driver start. Node numbers
// are reassigned by a bus reset; the counters stay with the number.
struct AsyncNodeLinkCounters final {
    std::atomic<uint32_t> requests{0};  // requests whose transmission completed
    std::atomic<uint32_t> ackErr{0};    // ack_data_error / ack_type_error: the node got a damaged packet
    std::atomic<uint32_t> busy{0};      // ack_busy after the controller's own retries
    std::atomic<uint32_t> noAck{0};     // evt_missing_ack: nothing came back
    std::atomic<uint32_t> timeouts{0};  // evt_timeout, or no response within the split timeout
    std::atomic<uint32_t> otherEvent{0};
};

struct AsyncLinkCounters final {
    static constexpr uint32_t kNodes = 64;
    std::array<AsyncNodeLinkCounters, kNodes> node{};

    [[nodiscard]] AsyncNodeLinkCounters& For(uint16_t nodeID) noexcept { return node[nodeID & 0x3FU]; }

    static void Add(std::atomic<uint32_t>& counter) noexcept {
        counter.fetch_add(1, std::memory_order_relaxed);
    }

    // The link's verdict on one transmitted request: the OHCI event code of
    // its AT completion (OHCI 1.1 Table 3-2).
    void CountCompletion(uint16_t nodeID, uint8_t eventCode) noexcept {
        auto& counters = For(nodeID);
        Add(counters.requests);
        switch (eventCode) {
        case 0x11: // ack_complete
        case 0x12: // ack_pending
        case 0x0F: // evt_flushed: a bus reset took it, not the link
            break;
        case 0x1D: // ack_data_error
        case 0x1E: // ack_type_error
            Add(counters.ackErr);
            break;
        case 0x14: // ack_busy_X
        case 0x15: // ack_busy_A
        case 0x16: // ack_busy_B
            Add(counters.busy);
            break;
        case 0x03: // evt_missing_ack
            Add(counters.noAck);
            break;
        case 0x0A: // evt_timeout
            Add(counters.timeouts);
            break;
        default:
            Add(counters.otherEvent);
            break;
        }
    }

    void CountSplitTimeout(uint16_t nodeID) noexcept { Add(For(nodeID).timeouts); }
};

} // namespace ASFW::Async
