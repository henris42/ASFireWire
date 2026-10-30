#pragma once

#include <cstdint>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#endif

#include "../Controller/ControllerTypes.hpp"

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
};

} // namespace ASFW::Driver
