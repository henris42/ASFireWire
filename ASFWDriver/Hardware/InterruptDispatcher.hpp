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

    // Whether an interrupt writes the shared status block at once. The app
    // polls the block and the watchdog rewrites it every 250 ms, so only a
    // bus reset or an unrecoverable error is written from the interrupt;
    // async activity only stamps its time.
    [[nodiscard]] static constexpr bool PublishesStatusAtOnce(uint32_t intEvent) noexcept {
        return (intEvent & (IntEventBits::kBusReset | IntEventBits::kUnrecoverableError)) != 0;
    }
};

} // namespace ASFW::Driver
