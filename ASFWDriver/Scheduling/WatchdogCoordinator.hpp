#pragma once
#include "../Common/TeardownCompletion.hpp"

#include <cstdint>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSSharedPtr.h>
#endif

namespace ASFW {
namespace Async {
class IAsyncSubsystemPort;
}
namespace Isoch {
class IsochReceiveContext;
class IsochTransmitContext;
} // namespace Isoch
} // namespace ASFW

class ASFWDriver;

namespace ASFW::Driver {
class ControllerCore;
class IsochLanes;
class StatusPublisher;

class WatchdogCoordinator {
  public:
    WatchdogCoordinator() = default;
    ~WatchdogCoordinator() = default;

    kern_return_t Prepare(::ASFWDriver& service, OSSharedPtr<IODispatchQueue> workQueue,
                                        std::shared_ptr<ASFW::Common::TeardownCompletion> completion = {});
    void Stop();
    void Reset();

    void Schedule(uint64_t delayUsec);

    void HandleTick(ControllerCore* controller, ASFW::Async::IAsyncSubsystemPort* asyncSubsystem,
                    ASFW::Isoch::IsochReceiveContext* isochReceiveContext,
                    ASFW::Isoch::IsochTransmitContext* isochTransmitContext,
                    StatusPublisher& statusPublisher);

    // The next tick: 1 ms while an isochronous context runs (the tick backs up
    // its interrupts and drains its telemetry), 10 ms otherwise, when the tick
    // only times out transactions and watches the AT contexts.
    [[nodiscard]] static uint64_t TickPeriodUsec(IsochLanes& lanes) noexcept;
    static constexpr uint64_t kStreamingTickUsec = 1'000;
    static constexpr uint64_t kIdleTickUsec = 10'000;

  private:
    std::shared_ptr<ASFW::Common::TeardownCompletion> teardownCompletion_;
    void TickAsyncSubsystem(ASFW::Async::IAsyncSubsystemPort* asyncSubsystem,
                            StatusPublisher& statusPublisher) const;
    void TickIsochReceive(ASFW::Isoch::IsochReceiveContext* isochReceiveContext);
    void TickIsochTransmit(ASFW::Isoch::IsochTransmitContext* isochTransmitContext);

    OSSharedPtr<IODispatchQueue> queue_;
    OSSharedPtr<IOTimerDispatchSource> timer_;
    OSSharedPtr<OSAction> action_;
    uint32_t ztsLogDivider_{0};
    uint32_t consumerDiagnosticsDivider_{0};
    // Mach time of the last status block write (the app polls it).
    uint64_t lastStatusPublishMach_{0};
    // Edge-triggered so a permanently ineligible drain reports once, not at
    // 1 kHz. Starts true: the interesting event is the drop to false.
    bool lastDrainEligible_{true};
};

} // namespace ASFW::Driver
