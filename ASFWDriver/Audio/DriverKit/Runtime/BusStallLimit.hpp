//
// BusStallLimit.hpp
// ASFWDriver
//
// The CPU idle limit an audio driver holds while its device runs IO
// (IOService::RequireMaxBusStall): the longest wake-up from an idle state the
// system may then choose, so the deep states stay out of use and a thread
// woken for the IO cycle (CoreAudio's IO thread in the client, AudioDriverKit's
// in the driver, the client's workers) starts at once. With the deep states in
// use those threads woke 100 us late at the median, and a client that needed
// 610 of the 760 us at 64 frames and 96 kHz overloaded 945 times a minute
// (b220, cores 54-61 % in C6); 27 us and 140 a minute with C1E as the deepest.
// The limit costs idle power while audio runs and nothing in the IO path: it
// is asked for at StartDevice and given up at StopDevice.
//
// One limit for the process, kDefaultBusStallNanos unless set through the
// user client (selectors 76/77, asfw-config-agent --bus-stall). A new limit
// holds from a device's next IO start. The kernel keeps one requirement per
// service and applies the strictest; its value and holder are the
// IOPlatformMaxBusDelay property in the registry.
//

#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// No requirement (kIOMaxBusStallNone).
inline constexpr uint32_t kBusStallNone = 0;
// The strictest the kernel names (kIOMaxBusStall5usec): C1 alone.
inline constexpr uint32_t kDefaultBusStallNanos = 5'000;

/// The values of DriverKit's kIOMaxBusStall enum (IOTypes.h).
[[nodiscard]] constexpr bool IsBusStallLimit(uint64_t nanos) noexcept {
    switch (nanos) {
    case kBusStallNone:
    case 5'000:
    case 10'000:
    case 20'000:
    case 25'000:
    case 30'000:
    case 40'000:
        return true;
    default:
        return false;
    }
}

namespace detail {
inline std::atomic<uint32_t> gBusStallNanos{kDefaultBusStallNanos};
inline std::atomic<uint32_t> gBusStallHolders{0};
} // namespace detail

/// The limit a device asks for at its next IO start.
[[nodiscard]] inline uint32_t BusStallLimitNanos() noexcept {
    return detail::gBusStallNanos.load(std::memory_order_relaxed);
}

/// False, and no change, for a value the kernel does not name.
inline bool SetBusStallLimitNanos(uint64_t nanos) noexcept {
    if (!IsBusStallLimit(nanos)) {
        return false;
    }
    detail::gBusStallNanos.store(static_cast<uint32_t>(nanos), std::memory_order_relaxed);
    return true;
}

/// How many devices hold a limit now.
[[nodiscard]] inline uint32_t BusStallHolders() noexcept {
    return detail::gBusStallHolders.load(std::memory_order_relaxed);
}

/// What one audio driver holds: 0 or the limit it asked the kernel for.
/// Begin returns the limit to ask for (kBusStallNone: none, the setting is
/// off); End returns true when a limit was held and is to be given up.
class BusStallHold {
public:
    [[nodiscard]] uint32_t Begin() noexcept {
        const uint32_t nanos = BusStallLimitNanos();
        const uint32_t before = held_.exchange(nanos, std::memory_order_acq_rel);
        if (before == kBusStallNone && nanos != kBusStallNone) {
            detail::gBusStallHolders.fetch_add(1, std::memory_order_relaxed);
        } else if (before != kBusStallNone && nanos == kBusStallNone) {
            detail::gBusStallHolders.fetch_sub(1, std::memory_order_relaxed);
        }
        return nanos;
    }

    [[nodiscard]] bool End() noexcept {
        if (held_.exchange(kBusStallNone, std::memory_order_acq_rel) == kBusStallNone) {
            return false;
        }
        detail::gBusStallHolders.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] uint32_t HeldNanos() const noexcept {
        return held_.load(std::memory_order_acquire);
    }

private:
    std::atomic<uint32_t> held_{kBusStallNone};
};

// User-client wire format, selectors 76 (set: the limit in nanoseconds as the
// first scalar) and 77 (get). Both answer with the limit set and how many
// devices hold one now.
struct __attribute__((packed)) BusStallWire {
    uint32_t limitNanos{0};
    uint32_t holders{0};
};
static_assert(sizeof(BusStallWire) == 8, "bus stall wire layout is frozen");

inline void CopyBusStallState(BusStallWire& out) noexcept {
    out.limitNanos = BusStallLimitNanos();
    out.holders = BusStallHolders();
}

} // namespace ASFW::Audio::Runtime
