// IsochLinkCounters.hpp
// ASFW - what the link reported for the packets of one receive stream.
//
// The controller writes a status word into every receive descriptor it
// completes (OHCI 1.1 §10.2.2: ContextControl[15:0] at completion). Its low
// five bits are the event code, bits 7:5 the speed the packet arrived at.
// A damaged packet is delivered like any other, marked ack_data_error, so the
// cable's bit errors are countable here and nowhere else.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace ASFW::Isoch {

enum class IsochRxStatusClass : uint8_t {
    kComplete,       // ack_complete (0x11); ack_pending (0x12) on some controllers
    kDataCrc,        // ack_data_error (0x1D): data CRC or data length wrong on the wire
    kLongPacket,     // evt_long_packet (0x02): more data than the buffer holds
    kOverrun,        // evt_overrun (0x05): the controller's receive FIFO overflowed
    kDescriptorRead, // evt_descriptor_read (0x06)
    kDataWrite,      // evt_data_write (0x08): host memory write failed
    kTimeout,        // evt_timeout (0x0A)
    kUnknown,        // evt_unknown (0x0E)
    kOther,
};

[[nodiscard]] constexpr IsochRxStatusClass ClassifyIsochRxStatus(uint16_t transferStatus) noexcept {
    switch (transferStatus & 0x1FU) {
    case 0x11:
    case 0x12:
        return IsochRxStatusClass::kComplete;
    case 0x1D:
        return IsochRxStatusClass::kDataCrc;
    case 0x02:
        return IsochRxStatusClass::kLongPacket;
    case 0x05:
        return IsochRxStatusClass::kOverrun;
    case 0x06:
        return IsochRxStatusClass::kDescriptorRead;
    case 0x08:
        return IsochRxStatusClass::kDataWrite;
    case 0x0A:
        return IsochRxStatusClass::kTimeout;
    case 0x0E:
        return IsochRxStatusClass::kUnknown;
    default:
        return IsochRxStatusClass::kOther;
    }
}

// Packet-per-buffer with isochHeader: timestamp quadlet, isochronous header
// quadlet (data_length in its upper 16 bits, host order), then the data.
inline constexpr size_t kIsochRxPrefixBytes = 8;

// True when the bytes the controller stored agree with the data_length the
// packet's own header carries. The data field is padded to a quadlet on the
// wire; a controller may or may not store the pad bytes.
[[nodiscard]] constexpr bool IsochRxLengthConsistent(const uint8_t* buffer, size_t bytes) noexcept {
    if (buffer == nullptr || bytes < kIsochRxPrefixBytes) {
        return false;
    }
    const size_t dataLength = static_cast<size_t>(buffer[6]) | (static_cast<size_t>(buffer[7]) << 8);
    const size_t stored = bytes - kIsochRxPrefixBytes;
    return stored == dataLength || stored == ((dataLength + 3U) & ~static_cast<size_t>(3U));
}

// One writer at a time (the receive poll gate); read from anywhere. Plain
// load/store pairs, no locked instruction on the packet path.
struct IsochRxLinkCounters final {
    // Transport: from the descriptor status of every packet.
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> dataCrc{0};
    std::atomic<uint64_t> longPacket{0};
    std::atomic<uint64_t> overrun{0};
    std::atomic<uint64_t> descriptorRead{0};
    std::atomic<uint64_t> dataWrite{0};
    std::atomic<uint64_t> timeout{0};
    std::atomic<uint64_t> unknown{0};
    std::atomic<uint64_t> otherStatus{0};
    std::atomic<uint64_t> lengthMismatch{0};
    std::atomic<uint32_t> lastBadStatus{0}; // whole status word of the newest non-complete packet
    std::atomic<uint32_t> lastSpeedCode{0}; // of the newest packet: 0 S100, 1 S200, 2 S400, 3 S800

    // Content: written by the stream's consumer, which knows the format.
    std::atomic<uint64_t> dbcGaps{0};          // packets whose block counter did not follow
    std::atomic<uint64_t> dbcBlocksMissed{0};  // data blocks those gaps skipped
    std::atomic<uint64_t> cyclesMissed{0};     // bus cycles without a packet of the stream
    std::atomic<uint64_t> cipChanges{0};       // SID, DBS, FMT or FDF differing from the packet before
    std::atomic<uint32_t> dbcUnusable{0};      // the device's counter does not follow IEC 61883-1

    static void Add(std::atomic<uint64_t>& counter, uint64_t n = 1) noexcept {
        counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }

    void Reset() noexcept {
        for (auto* counter : {&packets, &dataCrc, &longPacket, &overrun, &descriptorRead, &dataWrite,
                              &timeout, &unknown, &otherStatus, &lengthMismatch, &dbcGaps,
                              &dbcBlocksMissed, &cyclesMissed, &cipChanges}) {
            counter->store(0, std::memory_order_relaxed);
        }
        lastBadStatus.store(0, std::memory_order_relaxed);
        lastSpeedCode.store(0, std::memory_order_relaxed);
        dbcUnusable.store(0, std::memory_order_relaxed);
    }

    // One received packet's descriptor status and stored bytes.
    void CountPacket(uint16_t transferStatus, const uint8_t* buffer, size_t bytes) noexcept {
        Add(packets);
        const uint32_t speedCode = (transferStatus >> 5) & 0x7U;
        if (speedCode != lastSpeedCode.load(std::memory_order_relaxed)) {
            lastSpeedCode.store(speedCode, std::memory_order_relaxed);
        }
        const IsochRxStatusClass statusClass = ClassifyIsochRxStatus(transferStatus);
        if (statusClass == IsochRxStatusClass::kComplete) {
            if (!IsochRxLengthConsistent(buffer, bytes)) {
                Add(lengthMismatch);
            }
            return;
        }
        lastBadStatus.store(transferStatus, std::memory_order_relaxed);
        switch (statusClass) {
        case IsochRxStatusClass::kDataCrc:
            Add(dataCrc);
            break;
        case IsochRxStatusClass::kLongPacket:
            Add(longPacket);
            break;
        case IsochRxStatusClass::kOverrun:
            Add(overrun);
            break;
        case IsochRxStatusClass::kDescriptorRead:
            Add(descriptorRead);
            break;
        case IsochRxStatusClass::kDataWrite:
            Add(dataWrite);
            break;
        case IsochRxStatusClass::kTimeout:
            Add(timeout);
            break;
        case IsochRxStatusClass::kUnknown:
            Add(unknown);
            break;
        default:
            Add(otherStatus);
            break;
        }
    }
};

} // namespace ASFW::Isoch
