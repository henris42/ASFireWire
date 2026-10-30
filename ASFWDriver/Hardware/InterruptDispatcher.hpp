#pragma once

#include <cstdint>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#endif

#include "../Controller/ControllerTypes.hpp"
#include "RegisterMap.hpp"

namespace ASFW {
namespace Async {
class IAsyncSubsystemPort;
}
} // namespace ASFW

namespace ASFW::Driver {

class ControllerCore;
class HardwareInterface;
class IsochLanes;
class StatusPublisher;

class InterruptDispatcher {
  public:
    InterruptDispatcher() = default;
    ~InterruptDispatcher() = default;

    void HandleSnapshot(const InterruptSnapshot& snap, ControllerCore& controller,
                        HardwareInterface& hardware,
                        IsochLanes& lanes, StatusPublisher& statusPublisher,
                        ASFW::Async::IAsyncSubsystemPort* asyncSubsystem);

    // Whether an interrupt with these events republishes the shared status:
    // always for anything but isochronous completions, which change nothing
    // the status block shows; for those at most once per kIsochOnlyStatusNanos.
    // A publish wakes every status listener (the app redraws on each), and the
    // isochronous contexts interrupt about 2000 times a second with two units
    // streaming.
    [[nodiscard]] bool ShouldPublishStatus(uint32_t intEvent, uint64_t nowNanos) noexcept {
        constexpr uint32_t kIsochOnly = IntEventBits::kIsochTx | IntEventBits::kIsochRx;
        if ((intEvent & ~kIsochOnly) != 0) {
            return true;
        }
        if (lastIsochOnlyStatusNanos_ != 0 && nowNanos >= lastIsochOnlyStatusNanos_ &&
            nowNanos - lastIsochOnlyStatusNanos_ < kIsochOnlyStatusNanos) {
            return false;
        }
        lastIsochOnlyStatusNanos_ = nowNanos;
        return true;
    }
    static constexpr uint64_t kIsochOnlyStatusNanos = 250'000'000ULL;

  private:
    uint64_t lastIsochOnlyStatusNanos_{0};
};

} // namespace ASFW::Driver
