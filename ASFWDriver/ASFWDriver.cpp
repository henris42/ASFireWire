// SPDX-License-Identifier: Apache-2.0
//
//  ASFWDriver.cpp
//  ASFWDriver
//
//  Created by Alexander Shabelnikov on 21.09.2025.
//

#define _LIBCPP_NO_ABI_TAG 1
#include "Audio/Protocols/AVC/DiscoveryCoordinator.hpp"
#include "Common/DriverKitCompat.hpp"
#include <DriverKit/DriverKit.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOKitKeys.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOServiceNotificationDispatchSource.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSString.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include "Common/ActionTeardown.hpp"
#include <string>

#include <net.asfw.driver/ASFWDriver.h>           // generated from .iig
#include <net.asfw.driver/ASFWDriverUserClient.h> // generated from .iig

#include "Async/AsyncSubsystem.hpp"
#include "Async/DMAMemoryImpl.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Async/PacketHelpers.hpp"
#include "Async/ResponseCode.hpp"
#include "Audio/Core/AudioCoordinator.hpp"
#include "Audio/Core/AudioEndpointRuntime.hpp"
#include "Audio/Core/AudioRuntimeRegistry.hpp"
#include "Audio/Protocols/AVCStartReadiness.hpp"
#include "Audio/Protocols/IDeviceProtocol.hpp"
#include "Bus/BusResetCoordinator.hpp"
#include "Bus/SelfIDCapture.hpp"
#include "Common/DriverKitOwnership.hpp"
#include "ConfigROM/ConfigROMStager.hpp"
#include "ConfigROM/ROMReader.hpp"
#include "ConfigROM/ROMScanner.hpp"
#include "Controller/ControllerCore.hpp"
#include "Controller/ControllerStateMachine.hpp"
#include "Diagnostics/MetricsSink.hpp"
#include "Discovery/DeviceManager.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Discovery/FWDevice.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Hardware/InterruptDrain.hpp"
#include "Hardware/InterruptManager.hpp"
#include "Hardware/OHCIConstants.hpp"
#include "Hardware/RegisterMap.hpp"
#include "Bus/IRM/IRMClient.hpp"
#include "Isoch/Receive/IsochReceiveContext.hpp"
#include "Isoch/Transmit/IsochTransmitContext.hpp"
#include "Common/TimingUtils.hpp"
#include "Logging/LogConfig.hpp"
#include "Logging/Logging.hpp"
#include "Protocols/AVC/AVCDiscovery.hpp"
#include "Protocols/AVC/CMP/CMPClient.hpp"
#include "Protocols/AVC/FCPResponseRouter.hpp"
#include "Protocols/SBP2/Session/DriverKitSessionScheduler.hpp"
#include "Scheduling/Scheduler.hpp"
#include "Service/DriverContext.hpp"
#include "Service/LocalRequestWiring.hpp"
#include "SCSIController/SBP2BridgeHub.hpp"
#include "SCSIController/SBP2NubPublisher.hpp"
#include "SCSIController/SBP2TargetBridge.hpp"
#include "Shared/Memory/DMAMemoryManager.hpp"
#include "Version/DriverVersion.hpp"
#include <net.asfw.driver/ASFWAudioNub.h>

using namespace ASFW::Driver;

class ASFWDriverUserClient;

namespace {
constexpr uint64_t kAsyncWatchdogPeriodUsec = 1000; // 1 ms tick (hybrid: interrupt + timer backup)

[[nodiscard]] ASFW::IRM::IRMClient::LocalIRMAccess
MakeLocalIRMAccess(const std::shared_ptr<HardwareInterface>& hardware) {
    return ASFW::IRM::IRMClient::LocalIRMAccess{
        .read = [hardware](uint32_t selector) -> LocalCSRReadResult {
            if (!hardware) {
                return {LocalCSRLockResult::Status::HardwareUnavailable, 0};
            }
            return hardware->ReadLocalIRMResource(selector);
        },
        .compareSwap =
            [hardware](uint32_t selector,
                       uint32_t compareValue,
                       uint32_t newValue) -> LocalCSRLockResult {
            if (!hardware) {
                return {LocalCSRLockResult::Status::HardwareUnavailable, 0, false};
            }
            return hardware->CompareSwapLocalIRMResource(selector, compareValue, newValue);
        },
    };
}

#ifndef ASFW_HOST_TEST
void ArmProviderTerminationNotifications(ASFWDriver& driver, IOService* provider,
                                         ServiceContext& ctx) {
    uint64_t providerEntryId = 0;
    if (!provider || provider->GetRegistryEntryID(&providerEntryId) != kIOReturnSuccess ||
        providerEntryId == 0) {
        return;
    }

    auto matching = OSSharedPtr(OSDictionary::withCapacity(1), OSNoRetain);
    auto idNum = OSSharedPtr(OSNumber::withNumber(providerEntryId, 64), OSNoRetain);
    if (!matching || !idNum) {
        return;
    }

    matching->setObject(kIORegistryEntryIDKey, idNum.get());

    IOServiceNotificationDispatchSource* rawSource = nullptr;
    const kern_return_t notifyKr =
        IOServiceNotificationDispatchSource::Create(matching.get(), 0, ctx.workQueue.get(),
                                                   &rawSource);
    if (notifyKr != kIOReturnSuccess || rawSource == nullptr) {
        return;
    }

    auto source = OSSharedPtr(rawSource, OSNoRetain);

    OSAction* rawAction = nullptr;
    const kern_return_t actionKr = driver.CreateActionProviderNotificationReady(0, &rawAction);
    if (actionKr != kIOReturnSuccess || rawAction == nullptr) {
        return;
    }

    ctx.providerNotificationAction = OSSharedPtr(rawAction, OSNoRetain);
    ctx.providerNotifications = std::move(source);

    (void)ctx.providerNotifications->SetHandler(ctx.providerNotificationAction.get());
    (void)ctx.providerNotifications->SetEnableWithCompletion(true, nullptr);
    ASFW_LOG(Controller, "✅ Provider termination notifications armed (entryID=%llu)",
             providerEntryId);
}
#endif

// FCP / DICE / SBP-2 / CSR inbound request handlers are now registered centrally
// by ASFW::Service::WireLocalRequestDispatch (Service/LocalRequestWiring.cpp),
// which owns request tCodes 0x0/0x1/0x4/0x5 and routes by destination address.

void EnsureRomScanner(ServiceContext& ctx) {
    if (!ctx.deps.speedPolicy || !ctx.controller) {
        return;
    }

    if (!ctx.deps.romScanner) {
        OSSharedPtr<IODispatchQueue> discoveryQueue = nullptr;
        if (ctx.deps.scheduler) {
            discoveryQueue = ctx.deps.scheduler->Queue();
        }

        ASFW::Discovery::ROMScannerParams scannerParams{};
        ctx.deps.romScanner = std::make_shared<ASFW::Discovery::ROMScanner>(
            ctx.controller->Bus(), *ctx.deps.speedPolicy, scannerParams, discoveryQueue);
        ASFW_LOG(Controller, "✅ ROMScanner created");
    } else {
        ASFW_LOG(Controller, "Reusing existing ROMScanner instance");
    }

    if (ctx.deps.romScanner) {
        ctx.controller->AttachROMScanner(ctx.deps.romScanner);
    }
}

void ExecuteRuntimeTeardown(ServiceContext& ctx, const QuiescePlan& plan) {
    const bool providerRevoked = plan.reason == QuiesceReason::kProviderRevoked ||
                                 (ctx.deps.hardware && ctx.deps.hardware->HardwareGone());

    // A provider notification can arrive while another quiesce is in progress.
    // Revoke first so no later software teardown can enter an OHCI MMIO scope.
    if (providerRevoked && ctx.deps.hardware) {
        ctx.deps.hardware->LatchProviderRevokedAndDrain();
        ASFW_LOG(Controller,
                 "[Lifecycle] runtime teardown mode=provider-revoked reason=%u",
                 static_cast<uint32_t>(plan.reason));
    }

#ifndef ASFW_HOST_TEST
    ctx.DisarmProviderNotifications();
#endif

    if (ctx.deps.asyncSubsystem) {
        ctx.deps.asyncSubsystem->BeginQuiesce();
    }
    if (ctx.audioCoordinator) {
        ctx.audioCoordinator->BeginTeardown();
    }
    // DV capture holds an isoch receive consumer and a shared ring mapping, so it
    // must stop before buffers, mappings or hardware are released. In the merged
    // lifecycle this belongs here rather than in ServiceContext::Reset, which only
    // releases resources after the quiesce executor has stopped them.
    ctx.dvCapture.StopAll(ctx.isoch);
    if (ctx.deps.avcDiscovery) {
        ctx.deps.avcDiscovery->Shutdown();
    }
    ASFW::Protocols::SBP2::SBP2BridgeHub::Clear();
    if (ctx.sbp2Bridge) {
        ctx.sbp2Bridge->Shutdown();
        ctx.sbp2Bridge.reset();
    }
    if (ctx.sbp2NubPublisher && plan.reason != QuiesceReason::kSystemSuspend &&
        plan.reason != QuiesceReason::kWakeRebuild) {
        ctx.sbp2NubPublisher->Shutdown();
        ctx.sbp2NubPublisher.reset();
    }

    ctx.watchdog.Stop();
    // Quiet at the controller before the source goes: nothing of ours may
    // assert the line (or DMA) once the driver has left, including across a
    // warm reset of a card that keeps its state (b205 B760 report).
    if (!providerRevoked && ctx.deps.hardware && !ctx.deps.hardware->HardwareGone() &&
        plan.completedStartStage >= StartStage::kProviderOpened) {
        ctx.deps.hardware->SetInterruptMask(0xFFFFFFFFu, false);
        ctx.deps.hardware->ClearIntEvents(0xFFFFFFFFu);
    }
    if (ctx.deps.interrupts) {
        ctx.deps.interrupts->Disable();
        if (plan.reason != QuiesceReason::kSystemSuspend &&
            plan.reason != QuiesceReason::kWakeRebuild) {
            ctx.deps.interrupts->Teardown();
        }
    }
    // AudioCoordinator owns the neutral isoch session when it exists. Startup
    // failures before audio composition still need this direct fallback.
    if (!ctx.audioCoordinator) {
        (void)ctx.isochLanes.StopAll();
    }

    ctx.statusPublisher.Publish(ctx.controller.get(), ctx.deps.asyncController.get(),
                                SharedStatusReason::Disconnect);

    const bool hardwareGone = ctx.deps.hardware && ctx.deps.hardware->HardwareGone();
    // Surprise removal skips all final register cleanup. The teardown calls
    // above are software-safe and their old direct-MMIO helpers are revoked.
    if (!hardwareGone && plan.completedStartStage >= StartStage::kProviderOpened) {
        if (ctx.deps.selfId && ctx.deps.hardware) {
            ctx.deps.selfId->Disarm(*ctx.deps.hardware);
        }
        if (ctx.deps.configRomStager && ctx.deps.hardware) {
            ctx.deps.configRomStager->Teardown(*ctx.deps.hardware);
        }
        // Soft reset last: every DMA context stops, linkEnable and LPS go,
        // the card is as a cold boot leaves it (Linux ohci.c pci_remove and
        // suspend do the same).
        if (ctx.controller) {
            (void)ctx.controller->PerformSoftReset();
        }
    } else if (hardwareGone) {
        ASFW_LOG(Controller,
                 "[Lifecycle] runtime teardown hardware-gone action=skip-final-mmio-cleanup");
    }
    if (ctx.deps.selfId) {
        ctx.deps.selfId->ReleaseBuffers();
    }
    if (ctx.deps.asyncSubsystem) {
        ctx.deps.asyncSubsystem->Stop();
    }
    if (ctx.controller) {
        ctx.controller->Stop();
    }
    if (ctx.deps.hardware) {
        ctx.deps.hardware->Detach();
    }
}

void ReleaseQuiescedRuntime(ServiceContext& ctx, const QuiescePlan& plan) {
    if (!ctx.lifecycle || !plan.runTeardown) {
        return;
    }
    ctx.lifecycle->CompleteQuiesce(plan, "runtime teardown complete", mach_absolute_time());
    const auto finalState = ctx.lifecycle->CurrentState();
    // A provider revocation can race synchronous bring-up. Leave the stopped
    // graph intact until StartRuntime observes the failed completion and
    // returns; otherwise the callback would free objects still in use by that
    // stack frame.
    if (plan.stateBefore == ControllerState::kStarting) {
        return;
    }
    ctx.Reset(finalState == ControllerState::kSuspended ? ServiceContext::ResetMode::ForSuspend
                                                         : ServiceContext::ResetMode::Full);
}
} // namespace

bool ASFWDriver::init() {
    if (!super::init())
        return false;
    if (!ivars) {
        ivars = IONewZero(ASFWDriver_IVars, 1);
        if (!ivars)
            return false;
    }
    if (!ivars->context) {
        ivars->context = IONew(ServiceContext, 1);
        if (!ivars->context)
            return false;
        // IONew is raw IOMalloc — it does NOT run constructors. Placement-new so
        // ServiceContext's members are actually initialized (config defaults,
        // OSSharedPtr/shared_ptr/atomics, StatusPublisher/IsochService/...) instead
        // of relying on zero-filled pages. Paired with ~ServiceContext() in free().
        new (ivars->context) ServiceContext();
    }
    return true;
}

void ASFWDriver::free() {
    ASFW_LOG(Controller, "ASFWDriver: free()");
    if (ivars) {
        if (ivars->context) {
            ivars->context->Reset();
            ivars->context->~ServiceContext(); // pair with placement-new in init()
            IOSafeDeleteNULL(ivars->context, ServiceContext, 1);
        }
        IODelete(ivars, ASFWDriver_IVars, 1);
        ivars = nullptr;
    }
    super::free();
}

#if ASFW_REQUIRE_ARMING
namespace {
// Backport builds stay inert unless the controller carries an "asfw-armed"
// property (OpenCore DeviceProperties on its PCI path). A dext wedged in Start()
// can stall boot; removing the property disarms it without a rescue shell.
bool ProviderIsArmed(IOService* provider) {
    OSDictionary* properties = nullptr;
    if (provider == nullptr || provider->CopyProperties(&properties) != kIOReturnSuccess ||
        properties == nullptr) {
        return false;
    }
    const bool armed = properties->getObject("asfw-armed") != nullptr;
    properties->release();
    return armed;
}
} // namespace
#endif

namespace {
// The boot options on the controller (OpenCore DeviceProperties; a string
// arrives as data with a trailing NUL, a number as data or a number).
struct BootOptions {
    bool tracePublish{false}; // asfw-boot-trace present: publish every step
    uint32_t traceStepMs{0};  // pause after each; 0 records without pausing
    bool manualStart{false};
};

constexpr uint32_t kDefaultBootTraceStepMs = 100;

bool PropertyText(OSObject* value, char* out, size_t capacity) {
    const char* text = nullptr;
    size_t length = 0;
    if (auto* data = OSDynamicCast(OSData, value); data != nullptr) {
        text = static_cast<const char*>(data->getBytesNoCopy());
        length = data->getLength();
    } else if (auto* string = OSDynamicCast(OSString, value); string != nullptr) {
        text = string->getCStringNoCopy();
        length = string->getLength();
    }
    if (text == nullptr || length >= capacity) {
        return false;
    }
    memcpy(out, text, length);
    out[length] = 0;
    return true;
}

BootOptions ReadBootOptions(IOService* provider) {
    BootOptions options{};
    OSDictionary* properties = nullptr;
    if (provider == nullptr || provider->CopyProperties(&properties) != kIOReturnSuccess ||
        properties == nullptr) {
        return options;
    }
    if (OSObject* trace = properties->getObject("asfw-boot-trace"); trace != nullptr) {
        options.tracePublish = true;
        options.traceStepMs = kDefaultBootTraceStepMs;
        char text[16];
        if (auto* number = OSDynamicCast(OSNumber, trace); number != nullptr) {
            options.traceStepMs = static_cast<uint32_t>(number->unsigned32BitValue());
        } else if (PropertyText(trace, text, sizeof(text)) && text[0] >= '0' && text[0] <= '9') {
            options.traceStepMs = static_cast<uint32_t>(strtoul(text, nullptr, 10));
        } else if (auto* data = OSDynamicCast(OSData, trace);
                   data != nullptr && data->getLength() > 0 && data->getLength() <= 8) {
            uint64_t value = 0;
            memcpy(&value, data->getBytesNoCopy(), data->getLength());
            options.traceStepMs = static_cast<uint32_t>(value);
        }
        // 0 is a value: every step published, no pause (b199 B760 report: the
        // pauses moved the start past the window in which it hung).
    }
    char mode[16];
    if (OSObject* start = properties->getObject("asfw-start");
        start != nullptr && PropertyText(start, mode, sizeof(mode))) {
        options.manualStart = strcmp(mode, "manual") == 0;
    }
    properties->release();
    return options;
}

// BringUpTrace's publisher in lockstep: the kept lines go into this service's
// registry entry as asfw-boot-trace-lines (every kept line, oldest first) and
// asfw-boot-trace-count (how many there have been in all), where the boot
// trace daemon reads them (asfw-config-agent --boot-trace-daemon).
void PublishBootTraceLines(void* context, const char* const* lines, uint32_t count,
                           uint64_t total) {
    auto* driver = static_cast<ASFWDriver*>(context);
    auto array = OSSharedPtr(OSArray::withCapacity(count), OSNoRetain);
    auto properties = OSSharedPtr(OSDictionary::withCapacity(2), OSNoRetain);
    auto number = OSSharedPtr(OSNumber::withNumber(total, 64), OSNoRetain);
    if (!driver || !array || !properties || !number) {
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (auto line = OSSharedPtr(OSString::withCString(lines[index]), OSNoRetain)) {
            array->setObject(line.get());
        }
    }
    properties->setObject("asfw-boot-trace-lines", array.get());
    properties->setObject("asfw-boot-trace-count", number.get());
    (void)driver->SetProperties(properties.get());
}
} // namespace

kern_return_t IMPL(ASFWDriver, Start) {
    auto kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess)
        return kr;
    // The log ring and the log level first, so that every start step is
    // logged, at the level the asfw-log-level property asks for.
    ASFW::Logging::LogRing::Shared().Initialize();
    ASFW::Logging::InitializeLogConfig(provider);
#if ASFW_REQUIRE_ARMING
    if (!ProviderIsArmed(provider)) {
        ASFW_LOG_WARNING(Controller, "ASFWDriver::Start(): no asfw-armed property on the controller; not starting");
        return kIOReturnNotPermitted;
    }
#endif
    if (!ivars || !ivars->context)
        return kIOReturnNoMemory;
    ivars->powerProvider = provider;
    const BootOptions options = ReadBootOptions(provider);
    ivars->bootTracePublish = options.tracePublish;
    ivars->bootTraceStepMs = options.traceStepMs;
    ivars->manualStart = options.manualStart;
    if (options.tracePublish || options.manualStart) {
        // Registered now, before any hardware is touched, so the boot-trace
        // daemon finds this service while the bring-up runs and the agent can
        // open a user client on a controller that waits for --bring-up.
        RegisterService();
        ivars->serviceRegistered = true;
        ASFW_LOG_MILESTONE(Controller,
                           "[StartStep] boot options: trace %u ms per step%{public}s%{public}s",
                           options.traceStepMs, options.traceStepMs == 0 ? " (no pauses)" : "",
                           options.manualStart ? ", manual start" : "");
    }
    if (options.manualStart) {
        // The controller is ours (IOKit matched us, so Apple's driver stays
        // off it) and untouched: no PCI open, no register, no DMA, no
        // interrupt. asfw-config-agent --bring-up runs StartRuntime.
        // The agent and the app map the shared status block as soon as they
        // open a user client and give up on the connection when it is missing
        // (b196 on the B760: --bring-up found "no controller"); the block, a
        // plain buffer that StartRuntime would otherwise create, exists from
        // here on and reads as "no controller" until the bring-up publishes.
        ArmStartWatchdog();
        const kern_return_t statusKr = ivars->context->statusPublisher.Prepare();
        DisarmStartWatchdog();
        if (statusKr != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Controller, "[StartStep] manual start: status block prepare failed: 0x%08x",
                           statusKr);
            return statusKr;
        }
        ivars->context->statusPublisher.Publish(nullptr, nullptr, SharedStatusReason::Boot);
        ASFW_LOG_MILESTONE(Controller,
                           "[StartStep] manual start: the controller is claimed and left alone; run "
                           "asfw-config-agent --bring-up to bring it up");
        return kIOReturnSuccess;
    }
    // A Start() that blocks (b49: kmem_alloc_contig, b52: CreateMapping, both
    // kernel RPCs that never returned) holds the provider busy and the whole
    // boot with it; a Start() that fails lets IOKit fall back to Apple's
    // driver. The watchdog turns the former into the latter.
    ArmStartWatchdog();
    kr = StartRuntime(provider);
    DisarmStartWatchdog();
    return kr;
}

kern_return_t ASFWDriver::RequestBringUp() {
    if (!ivars || !ivars->powerProvider) {
        return kIOReturnNotReady;
    }
    if (__atomic_load_n(&ivars->startInProgress, __ATOMIC_ACQUIRE)) {
        return kIOReturnBusy;
    }
    IODispatchQueue* queue = nullptr;
    if (CopyDispatchQueue(kIOServiceDefaultQueueName, &queue) != kIOReturnSuccess || !queue) {
        return kIOReturnError;
    }
    retain();
    queue->DispatchAsync(^{
        ASFW_LOG_MILESTONE(Controller, "[StartStep] bring-up requested (asfw-config-agent --bring-up)");
        ArmStartWatchdog();
        const kern_return_t kr = StartRuntime(ivars->powerProvider);
        DisarmStartWatchdog();
        ASFW_LOG_MILESTONE(Controller, "[StartStep] bring-up on request: 0x%08x", kr);
        release();
    });
    queue->release();
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::RequestBringDown() {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }
    IODispatchQueue* queue = nullptr;
    if (CopyDispatchQueue(kIOServiceDefaultQueueName, &queue) != kIOReturnSuccess || !queue) {
        return kIOReturnError;
    }
    retain();
    queue->DispatchAsync(^{
        // The sleep path's quiesce: the runtime goes, the service stays, and
        // StartRuntime brings it back (as a wake does).
        ASFW_LOG_MILESTONE(Controller, "[StartStep] bring-down requested (asfw-config-agent --bring-down)");
        RequestRuntimeQuiesce(static_cast<uint32_t>(QuiesceReason::kSystemSuspend));
        ASFW_LOG_MILESTONE(Controller, "[StartStep] bring-down on request done");
        release();
    });
    queue->release();
    return kIOReturnSuccess;
}

namespace {
// Normal Start() is well under a few seconds (soft reset, LPS, bus reset,
// self-ID); a stuck RPC never returns. Long enough for a slow VM, short
// enough that the boot continues without a power reset. In lockstep every
// step pauses, and the register details make hundreds of them.
constexpr uint64_t kStartWatchdogNs = 30ull * 1000ull * 1000ull * 1000ull;
constexpr uint64_t kStartWatchdogLockstepNs = 600ull * 1000ull * 1000ull * 1000ull;
} // namespace

void ASFWDriver::ArmStartWatchdog() {
    if (!ivars) {
        return;
    }
    __atomic_store_n(&ivars->startInProgress, true, __ATOMIC_RELEASE);
    if (!ivars->startWatchdogTimer) {
        IODispatchQueue* queue = nullptr;
        kern_return_t kr = IODispatchQueue::Create("ASFW.start-watchdog", 0, 0, &queue);
        if (kr != kIOReturnSuccess || !queue) {
            ASFW_LOG_ERROR(Controller, "Start watchdog: queue create failed: 0x%08x", kr);
            return;
        }
        IOTimerDispatchSource* timer = nullptr;
        kr = IOTimerDispatchSource::Create(queue, &timer);
        if (kr != kIOReturnSuccess || !timer) {
            ASFW_LOG_ERROR(Controller, "Start watchdog: timer create failed: 0x%08x", kr);
            queue->release();
            return;
        }
        OSAction* action = nullptr;
        kr = CreateActionStartWatchdogFired(0, &action);
        if (kr != kIOReturnSuccess || !action) {
            ASFW_LOG_ERROR(Controller, "Start watchdog: action create failed: 0x%08x", kr);
            timer->release();
            queue->release();
            return;
        }
        kr = timer->SetHandler(action);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Controller, "Start watchdog: SetHandler failed: 0x%08x", kr);
            action->release();
            timer->release();
            queue->release();
            return;
        }
        (void)timer->SetEnableWithCompletion(true, nullptr);
        ivars->startWatchdogQueue = queue;
        ivars->startWatchdogTimer = timer;
        ivars->startWatchdogAction = action;
    }
    (void)ASFW::Timing::initializeHostTimebase();
    const uint64_t budgetNs = ivars->bootTraceStepMs != 0 ? kStartWatchdogLockstepNs : kStartWatchdogNs;
    const uint64_t deadline = mach_absolute_time() + ASFW::Timing::nanosToHostTicks(budgetNs);
    (void)ivars->startWatchdogTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, deadline, 0);
    ASFW_LOG(Controller, "Start watchdog armed: %llu s", budgetNs / 1000000000ull);
}

void ASFWDriver::DisarmStartWatchdog() {
    if (!ivars) {
        return;
    }
    __atomic_store_n(&ivars->startInProgress, false, __ATOMIC_RELEASE);
    if (!ivars->startWatchdogTimer) {
        return;
    }
    auto* timer = ivars->startWatchdogTimer;
    auto* action = ivars->startWatchdogAction;
    auto* queue = ivars->startWatchdogQueue;
    ivars->startWatchdogTimer = nullptr;
    ivars->startWatchdogAction = nullptr;
    ivars->startWatchdogQueue = nullptr;
    (void)timer->SetEnableWithCompletion(false, nullptr);
    // Cancel is terminal; the action and its queue go once every queued
    // callback has run (ActionTeardown.hpp).
    const kern_return_t kr = timer->Cancel(^{
        timer->release();
        ASFW::Common::CancelAndReleaseOwnedAction(action, "start watchdog",
                                                   [queue]() { queue->release(); }, queue);
    });
    if (kr != kIOReturnSuccess) {
        timer->release();
        ASFW::Common::CancelAndReleaseOwnedAction(action, "start watchdog",
                                                   [queue]() { queue->release(); }, queue);
    }
    ASFW_LOG(Controller, "Start watchdog disarmed");
}

void ASFWDriver::StartWatchdogFired_Impl(ASFWDriver_StartWatchdogFired_Args) {
    (void)action;
    (void)time;
    if (!ivars || !__atomic_load_n(&ivars->startInProgress, __ATOMIC_ACQUIRE)) {
        return;
    }
    ASFW_LOG_ERROR(Controller,
                   "❌ Start() has not returned after %llu s; it is stuck in the kernel. Ending the "
                   "process so IOKit treats the start as failed and Apple's driver takes the card.",
                   kStartWatchdogNs / 1000000000ull);
    IOSleep(300); // give the log line a chance to leave the process
    __builtin_trap();
}

namespace {
// The platform expert keeps IOPlatformMapperPresent only while an IOMMU remaps
// DMA: AppleVTD removes it when it does not start (IOPCIFamily vtd.c), as does
// the dart=0 boot-arg (xnu IOPlatformExpert.cpp).
const char* IommuState(IOService* provider) {
    OSContainer* value = nullptr;
    if (provider == nullptr ||
        provider->SearchProperty("IOPlatformMapperPresent", kIOServicePlane,
                                 kIOServiceSearchPropertyParents, &value) != kIOReturnSuccess ||
        value == nullptr) {
        return "off";
    }
    value->release();
    return "on";
}
} // namespace

kern_return_t ASFWDriver::StartRuntime(IOService* provider) {
    if (!ivars || !ivars->context)
        return kIOReturnNoMemory;
    kern_return_t kr = kIOReturnSuccess;
    auto& ctx = *ivars->context;
    DriverWiring::EnsureDeps(this, ctx);
    if (!ctx.lifecycle || !ctx.lifecycle->BeginStart("runtime start", mach_absolute_time())) {
        return kIOReturnBusy;
    }
    ctx.lifecycle->MarkStageComplete(StartStage::kDependenciesReady);
    const auto failStart = [&ctx](kern_return_t status, const char* detail) {
        ASFW_LOG_ERROR(Controller, "ASFWDriver::Start() failed at %{public}s: 0x%08x", detail, status);
        if (ctx.deps.hardware) {
            ctx.deps.hardware->BringUpTrace().Step("start failed at %s: 0x%08x", detail, status);
            ctx.deps.hardware->BringUpTrace().End("start failed");
        }
        if (ctx.lifecycle) {
            if (const auto plan = ctx.lifecycle->BeginFailedStart(detail, mach_absolute_time())) {
                if (plan->runTeardown) {
                    ExecuteRuntimeTeardown(ctx, *plan);
                    ReleaseQuiescedRuntime(ctx, *plan);
                }
            }
        }
        if (ctx.lifecycle && ctx.lifecycle->CurrentState() == ControllerState::kStopped) {
            ctx.Reset(ServiceContext::ResetMode::Full);
        }
        return status;
    };
    if (auto statusKr = ctx.statusPublisher.Prepare(); statusKr != kIOReturnSuccess) {
        return failStart(statusKr, "status publisher prepare failed");
    }
    kr = DriverWiring::PrepareQueue(*this, ctx);
    if (kr != kIOReturnSuccess) {
        return failStart(kr, "dispatch queue prepare failed");
    }
    ctx.lifecycle->MarkStageComplete(StartStage::kQueueReady);
    auto& trace = ctx.deps.hardware->BringUpTrace();
    if (ivars->bootTracePublish && !trace.Lockstep()) {
        trace.SetLockstep(ivars->bootTraceStepMs, &PublishBootTraceLines, this);
    }
    trace.Begin();
    trace.Step("start %s (%s%s), IOMMU %s%s", ASFW::Version::kSemanticVersion,
               ASFW::Version::kGitCommitShort, ASFW::Version::kGitDirty ? " dirty" : "",
               IommuState(provider), trace.Lockstep() ? ", lockstep trace" : "");

    kr = ctx.deps.hardware->Attach(this, provider);
    if (kr != kIOReturnSuccess) {
        return failStart(kr, "provider attach failed");
    }
    ctx.lifecycle->MarkStageComplete(StartStage::kProviderOpened);

    // Populate the shared host timebase before any interrupt can fire. The
    // InterruptOccurred handler converts the DriverKit mach-tick timestamp to
    // nanoseconds via ASFW::Timing::hostTicksToNanos(), which needs
    // gHostTimebaseInfo initialized. Bus-reset IRQs arrive long before the isoch
    // paths that were previously the only initializers of it.
    (void)ASFW::Timing::initializeHostTimebase();

    kr = DriverWiring::PrepareInterrupts(*this, provider, ctx);
    if (kr != kIOReturnSuccess) {
        return failStart(kr, "interrupt preparation failed");
    }
    ctx.lifecycle->MarkStageComplete(StartStage::kInterruptSourceReady);
    trace.Step("async DMA setup");

    // Initialize AsyncSubsystem (requires hardware, workQueue, and a completion action)
    if (ctx.deps.asyncSubsystem && ctx.deps.hardware && ctx.workQueue && ctx.interruptAction) {
        kr = ctx.deps.asyncSubsystem->Start(*ctx.deps.hardware, this, ctx.workQueue.get(),
                                            ctx.interruptAction.get(), size_t{64} * 1024u,
                                            ctx.teardownCompletion);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Controller, "AsyncSubsystem::Start() failed: 0x%08x", kr);
            return failStart(kr, "async subsystem start failed");
        }
    }
    ctx.lifecycle->MarkStageComplete(StartStage::kAsyncReady);
    trace.Step("async DMA ready");

    kr = DriverWiring::PrepareWatchdog(*this, ctx);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Controller, "Failed to prepare async watchdog: 0x%08x", kr);
        return failStart(kr, "watchdog preparation failed");
    }
    ScheduleAsyncWatchdog(kAsyncWatchdogPeriodUsec);

    // Count the isoch contexts now, while every context is idle: the probe
    // writes their CommandPtr registers (HardwareInterface::ProbeIsochContextCount).
    if (ctx.deps.hardware) {
        (void)ctx.isochLanes.LaneCount(*ctx.deps.hardware);
    }

    ctx.controller = std::make_shared<ControllerCore>(ctx.config, ctx.rolePolicy, ctx.deps);

    // FCP shares the driver's cancellable control-plane timer with SBP-2. It
    // must exist before AV/C discovery constructs per-unit FCP transports.
    kr = DriverWiring::EnsureSbp2Deps(*this, ctx);
    if (kr != kIOReturnSuccess) {
        return failStart(kr, "SBP-2 dependency preparation failed");
    }

    if (!ctx.deps.avcDiscovery && ctx.deps.deviceManager && ctx.deps.deviceRegistry) {
        auto& bus = ctx.controller->Bus();
        ctx.deps.avcDiscovery = std::make_shared<ASFW::Protocols::AVC::AVCDiscovery>(
            this, *ctx.deps.deviceRegistry, *ctx.deps.deviceManager, bus, bus, *ctx.deps.sbp2SessionScheduler,
            std::make_shared<ASFW::Audio::AVC::DiscoveryCoordinator>(
                *ctx.deps.deviceRegistry, bus, bus, ctx.audioCoordinator.get()));
        ctx.controller->SetAVCDiscovery(ctx.deps.avcDiscovery);
        ASFW_LOG(Controller, "✅ AVCDiscovery initialized");
    }

    if (!ctx.deps.fcpResponseRouter && ctx.deps.avcDiscovery) {
        ctx.deps.fcpResponseRouter =
            std::make_shared<ASFW::Protocols::AVC::FCPResponseRouter>(*ctx.deps.avcDiscovery);
        ctx.controller->SetFCPResponseRouter(ctx.deps.fcpResponseRouter);
        ASFW_LOG(Controller, "✅ FCPResponseRouter initialized");
    }

    // Assemble the single inbound request dispatch (CSR / FCP / DICE / SBP-2)
    // after all control-plane responders have been constructed.
    ASFW::Service::WireLocalRequestDispatch(ctx);
    EnsureRomScanner(ctx);

    if (ctx.deps.busReset) {
        // Recovery that has given up must end the driver, not leave it spinning
        // or wedged: Terminate() runs Stop() and the normal teardown.
        ctx.deps.busReset->BindFatalCallback([this, &ctx](const char* reason) {
            if (ctx.selfTerminationRequested.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            const kern_return_t terminateKr = Terminate(0);
            ASFW_LOG_ERROR(Controller, "❌ Fatal: %{public}s; terminating (Terminate -> 0x%08x)",
                           reason, terminateKr);
        });
    }

    trace.Step("controller start");
    kr = ctx.controller->Start(provider);
    if (kr != kIOReturnSuccess) {
        return failStart(kr, "controller start failed");
    }
    ctx.lifecycle->MarkStageComplete(StartStage::kControllerReady);

    if (!ctx.deps.irmClient) {
        ctx.deps.irmClient = std::make_shared<ASFW::IRM::IRMClient>(
            ctx.controller->Bus(),
            MakeLocalIRMAccess(ctx.deps.hardware));
        ctx.controller->SetIRMClient(ctx.deps.irmClient);
        ASFW_LOG(Controller, "✅ IRMClient initialized");
    }

    if (!ctx.deps.cmpClient) {
        if (!ctx.deps.deviceRegistry) {
            return failStart(kIOReturnNotReady, "device registry unavailable for CMP");
        }
        ctx.deps.cmpClient = std::make_shared<ASFW::CMP::CMPClient>(ctx.controller->Bus(),
                                                                      ctx.controller->Bus(),
                                                                      *ctx.deps.deviceRegistry);
        ctx.controller->SetCMPClient(ctx.deps.cmpClient);
        ASFW_LOG(Controller, "✅ CMPClient initialized");
    }

    if (ctx.audioCoordinator) {
        ctx.audioCoordinator->SetCMPClient(ctx.deps.cmpClient.get());
        ctx.audioCoordinator->SetIRMClient(ctx.deps.irmClient.get());
    }

    ctx.statusPublisher.Publish(ctx.controller.get(), ctx.deps.asyncController.get(),
                                SharedStatusReason::Boot);

    const uint32_t initialMask = IntMaskBits::kMasterIntEnable | kBaseIntMask;
    trace.Step("IntMaskSet 0x%08x", initialMask);
    ctx.deps.hardware->IntMaskSet(initialMask);

    // Register once per service instance: StartRuntime() is re-entered on wake.
    // SBP-2 nubs are published separately and only for discovered SBP-2 units.
    if (!ivars->serviceRegistered) {
        RegisterService();
        ivars->serviceRegistered = true;
    }

    if (!ctx.lifecycle->CompleteStart("runtime start complete", mach_absolute_time())) {
        return failStart(kIOReturnError, "runtime start completion rejected");
    }
    ctx.irqEntriesAtStart = ctx.irqEntries.load(std::memory_order_acquire);
    ctx.startCompleteNs = ASFW::Timing::hostTicksToNanos(mach_absolute_time());
    ctx.irqArrivalChecked = false;

#ifndef ASFW_HOST_TEST
    // Arm only after the last failStart(). Enabling the source queues an initial
    // delivery for the already-matching provider; a failed start that tears the
    // source down with that delivery still queued lets DriverKit invoke the
    // freed OSAction (seen on DriverKit 21: DidAction on 0xeeee... after the
    // interrupt setup failed). Arming after Attach also means a revocation can
    // only fence an already-open hardware incarnation.
    ArmProviderTerminationNotifications(*this, provider, ctx);
#endif

    // NOTE: do NOT call ChangePowerState/SetPowerOverride here. The kernel
    // joins a dext into the PM tree only after Start() returns
    // (xnu IOUserServer.cpp serviceStarted -> serviceJoinPMTree), so PM calls
    // made during Start() are dropped: powerOverrideOnPriv returns
    // IOPMNotYetInitialized (surfaced as kIOReturnError) and
    // ChangePowerState_Impl silently discards the same failure. The power
    // desire is pinned in SetPowerState() on the first On callback instead,
    // which the kernel delivers right after the PM join.

    ASFW_LOG(Controller, "ASFWDriver::Start() complete");
    trace.Step("start complete");

    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWDriver, Stop) {
    if (ivars && ivars->stopPending) {
        return kIOReturnSuccess;
    }
    if (ivars) ivars->stopPending = true;
    auto completion = ivars && ivars->context
        ? ivars->context->teardownCompletion
        : std::make_shared<ASFW::Common::TeardownCompletion>();
    // Keep the service/provider valid until the deferred superclass Stop.
    // No synchronous wait: cancellation completions need the Default queue.
    retain();
    provider->retain();
    ASFW_LOG(Controller, "[Teardown] ASFWDriver Stop begin driver=%p", this);
    // Stop is also delivered on surprise PCI removal. The provider's
    // Terminated notification can still be queued on this same dispatch
    // queue, so waiting for it before revoking BAR access is too late.
    // Terminal service Stop must use the existing software-only teardown;
    // planned runtime quiesce while the provider is alive is separate.
    if (ivars && ivars->context && ivars->context->deps.hardware) {
        ivars->context->deps.hardware->LatchProviderRevokedAndDrain();
    }
    RequestRuntimeQuiesce(static_cast<uint32_t>(QuiesceReason::kProviderRevoked));
    ASFW_LOG(Controller, "[Teardown] ASFWDriver Stop runtime quiesce returned");
    if (ivars) {
        if (ivars->wakeVerifyTimer) {
            // Final stop is terminal.  DriverKit retains the timer's action
            // until cancellation, and invokes this completion only after any
            // queued timer callback returns.
            auto* timer = ivars->wakeVerifyTimer;
            auto* action = ivars->wakeVerifyAction;
            IODispatchQueue* queue = ivars->context ? ivars->context->workQueue.get() : nullptr;
            ivars->wakeVerifyTimer = nullptr;
            ivars->wakeVerifyAction = nullptr;
            ASFW_LOG(Controller, "[Teardown] wake verify cancel requested source=%p action=%p", timer, action);
            auto done = completion->Begin();
            const kern_return_t kr = timer->Cancel(^{
                ASFW_LOG(Controller, "[Teardown] wake verify cancel completed source=%p action=%p", timer, action);
                timer->release();
                ASFW::Common::CancelAndReleaseOwnedAction(action, "wake verify", done, queue);
            });
            if (kr != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Controller, "[Teardown] wake verify cancel failed kr=0x%x", kr);
                timer->release();
                ASFW::Common::CancelAndReleaseOwnedAction(action, "wake verify", done, queue);
            }
        }
        ivars->powerProvider = nullptr;
    }
    ASFW_LOG(Controller, "[Teardown] ASFWDriver Stop waiting for cancellation drain");
    completion->FinishWhenDrained([this, provider] {
        ASFW_LOG(Controller, "[Teardown] ASFWDriver cancellations drained; calling superclass Stop");
        const auto kr = Stop(provider, SUPERDISPATCH);
        ASFW_LOG(Controller, "[Teardown] ASFWDriver Stop complete kr=0x%x", kr);
        provider->release();
        release();
    });
    return kIOReturnSuccess;
}

void ASFWDriver::RequestRuntimeQuiesce(uint32_t rawReason) {
    if (!ivars || !ivars->context) {
        return;
    }
    auto& ctx = *ivars->context;
    if (!ctx.lifecycle) {
        return;
    }

    const auto reason = static_cast<QuiesceReason>(rawReason);
    const auto plan = ctx.lifecycle->BeginQuiesce(reason, "runtime quiesce", mach_absolute_time());
    if (!plan.has_value()) {
        return;
    }

    // A revocation request that races an already-running planned teardown must
    // still fence OHCI immediately. The active executor will observe its
    // state as Revoked before publishing its final transition.
    if (plan->revokeImmediately && !plan->runTeardown && ctx.deps.hardware) {
        ctx.deps.hardware->LatchProviderRevokedAndDrain();
    }
    if (plan->runTeardown) {
        ExecuteRuntimeTeardown(ctx, *plan);
        ReleaseQuiescedRuntime(ctx, *plan);
    }
}

// Wake verification cadence. 3s puts the first check well past the dark-wake →
// full-wake transition (~2s observed) while staying invisible to the user; 5
// attempts bound the self-heal at ~15s.
static constexpr uint64_t kWakeVerifyDelayNs = 3'000'000'000ull;
static constexpr uint64_t kWakeVerifyMaxAttempts = 5;

kern_return_t IMPL(ASFWDriver, SetPowerState) {
    const bool poweredOn = (powerFlags & kIOServicePowerCapabilityOn) != 0;
    ASFW_LOG(Controller, "SetPowerState: powerFlags=0x%08x (%{public}s)", powerFlags,
             poweredOn ? "on" : "sleep/low");

    if (ivars) {
        if (!poweredOn) {
            // Sleep: quiesce everything and reset the runtime while the
            // controller still answers MMIO. The silicon loses its programmed
            // state in low power (Linux ohci.c pci_suspend does software_reset;
            // Apple gates all hardware access while asleep).
            if (ivars->context && ivars->context->lifecycle &&
                ivars->context->lifecycle->CurrentState() == ControllerState::kRunning) {
                ASFW_LOG(Controller, "SetPowerState: quiescing runtime for sleep");
                RequestRuntimeQuiesce(static_cast<uint32_t>(QuiesceReason::kSystemSuspend));
            }
        } else {
            // Pin our power desire to full-on. A bus controller must stay
            // powered even with no devices attached (plug detection needs a
            // programmed, interrupting controller). The audio driver matched on
            // our nub is a PM-tree child; when the last nub terminates, the
            // child's demand vanishes and the system sends SetPowerState(0)
            // ~1ms later — which tore down the runtime, leaving the controller
            // dead until the PM domain happened to repower minutes later.
            // SetPowerOverride makes our power state governed solely by our own
            // desire (children ignored), so capability 0 then means real system
            // sleep only. These calls only work once the PM join has happened
            // (after Start() returns) — this callback is the earliest reliable
            // point. Idempotent, so unconditional on every On is fine.
            const kern_return_t pmKr = ChangePowerState(kIOServicePowerCapabilityOn);
#if ASFW_HAS_SET_POWER_OVERRIDE
            const kern_return_t ovKr = SetPowerOverride(true);
#else
            // Older runtimes lack SetPowerOverride (Common/DriverKitCompat.hpp).
            // Without it the last nub going away can still power the controller off.
            const kern_return_t ovKr = kIOReturnUnsupported;
#endif
            ASFW_LOG(Controller,
                     "SetPowerState: pin desire On -> 0x%08x, override -> 0x%08x",
                     pmKr, ovKr);
        }
        if (poweredOn && ivars->context && ivars->context->lifecycle &&
            ivars->context->lifecycle->CurrentState() == ControllerState::kSuspended) {
            // Wake: rebuild the runtime from scratch — full OHCI re-init ending
            // in a forced bus reset, after which normal discovery re-publishes
            // devices (Linux pci_resume runs the same ohci_enable as cold probe).
            if (ivars->powerProvider) {
                ASFW_LOG(Controller, "SetPowerState: wake - rebuilding runtime");
                const kern_return_t kr = StartRuntime(ivars->powerProvider);
                if (kr != kIOReturnSuccess) {
                    ASFW_LOG_ERROR(Controller,
                                   "SetPowerState: ❌ wake runtime rebuild failed: 0x%08x", kr);
                } else {
                    // The On callback can arrive during dark wake; verify the
                    // rebuild actually took once the platform has settled.
                    ScheduleWakeVerify(1);
                }
            } else {
                ASFW_LOG(Controller, "SetPowerState: wake with no provider; skipping rebuild");
            }
        }
    }

    return SetPowerState(powerFlags, SUPERDISPATCH);
}

void ASFWDriver::VerifyWakeRuntime(uint64_t attempt) {
    if (!ivars || !ivars->context || !ivars->context->lifecycle ||
        !ivars->context->lifecycle->AdmitsNormalWork()) {
        return; // slept again (or tearing down) before the check fired
    }
    auto& ctx = *ivars->context;
    if (!ctx.deps.hardware || !ctx.deps.busReset) {
        return;
    }

    // The wake rebuild always ends in a forced bus reset, and resetCount only
    // advances via the full interrupt path (IRQ → Self-ID → coordinator). A
    // completed reset therefore proves interrupt delivery end to end.
    const uint32_t resets = ctx.deps.busReset->Metrics().resetCount;
    uint32_t hcControl = 0;
    uint32_t intEvent = 0;
    {
        auto access = ctx.deps.hardware->TryBeginAccess();
        if (!access) {
            return;
        }
        hcControl = access.Read(Register32::kHCControl);
        intEvent = access.Read(Register32::kIntEvent);
    }
    const bool mmioAlive = (hcControl != 0xFFFFFFFFu);
    const bool linkEnabled = mmioAlive && (hcControl & HCControlBits::kLinkEnable);

    if (resets > 0 && linkEnabled) {
        ASFW_LOG(Controller, "Wake verify: ✅ alive (resets=%u HCControl=0x%08x attempt=%llu)",
                 resets, hcControl, attempt);
        return;
    }

    // Distinguish the failure mode for the log: busReset pending in IntEvent
    // with resetCount==0 means the reset happened but the IRQ never arrived
    // (interrupt path dead); linkEnable clear means the controller was reset
    // under us after the rebuild; 0xFFFFFFFF means MMIO itself is gone.
    if (!mmioAlive) {
        intEvent = 0;
    }
    ASFW_LOG_WARNING(Controller,
                     "Wake verify: ❌ dead controller (resets=%u HCControl=0x%08x IntEvent=0x%08x "
                     "attempt=%llu/%llu) - rebuilding",
                     resets, hcControl, intEvent, attempt, kWakeVerifyMaxAttempts);

    if (attempt >= kWakeVerifyMaxAttempts) {
        ASFW_LOG_ERROR(Controller, "Wake verify: ❌ giving up after %llu attempts", attempt);
        return;
    }
    if (!ivars->powerProvider) {
        ASFW_LOG_ERROR(Controller, "Wake verify: no provider; cannot rebuild");
        return;
    }

    RequestRuntimeQuiesce(static_cast<uint32_t>(QuiesceReason::kWakeRebuild));
    const kern_return_t kr = StartRuntime(ivars->powerProvider);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Controller, "Wake verify: ❌ rebuild failed: 0x%08x", kr);
    }
    ScheduleWakeVerify(attempt + 1);
}

void ASFWDriver::ScheduleWakeVerify(uint64_t attempt) {
    if (!ivars || !ivars->context) {
        return;
    }
    if (!ivars->wakeVerifyTimer) {
        // ctx.workQueue is the service's default queue (DriverWiring::
        // PrepareQueue), so the timer stays valid across runtime rebuilds and
        // the verify serializes with Start/Stop/SetPowerState.
        auto& queue = ivars->context->workQueue;
        if (!queue) {
            return;
        }
        IOTimerDispatchSource* timer = nullptr;
        kern_return_t kr = IOTimerDispatchSource::Create(queue.get(), &timer);
        if (kr != kIOReturnSuccess || !timer) {
            ASFW_LOG_ERROR(Controller, "Wake verify: ❌ timer create failed: 0x%08x", kr);
            return;
        }
        OSAction* action = nullptr;
        kr = CreateActionWakeVerifyTimerFired(0, &action);
        if (kr != kIOReturnSuccess || !action) {
            ASFW_LOG_ERROR(Controller, "Wake verify: ❌ timer action create failed: 0x%08x", kr);
            timer->release();
            return;
        }
        kr = timer->SetHandler(action);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Controller, "Wake verify: ❌ timer SetHandler failed: 0x%08x", kr);
            action->release();
            timer->release();
            return;
        }
        (void)timer->SetEnableWithCompletion(true, nullptr);
        ivars->wakeVerifyTimer = timer;
        ivars->wakeVerifyAction = action;
    }

    ivars->wakeVerifyAttempt = attempt;
    (void)ASFW::Timing::initializeHostTimebase();
    const uint64_t deadline =
        mach_absolute_time() + ASFW::Timing::nanosToHostTicks(kWakeVerifyDelayNs);
    (void)ivars->wakeVerifyTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, deadline, 0);
}

void ASFWDriver::WakeVerifyTimerFired_Impl(ASFWDriver_WakeVerifyTimerFired_Args) {
    if (!ivars) {
        return;
    }
    VerifyWakeRuntime(ivars->wakeVerifyAttempt);
}

kern_return_t ASFWDriver::CopyControllerStatus(OSDictionary** status) {
    if (!status)
        return kIOReturnBadArgument;
    *status = nullptr;
    auto dict = OSDictionary::withCapacity(4);
    if (!dict)
        return kIOReturnNoMemory;
    if (ivars && ivars->context && ivars->context->controller) {
        auto& controller = *ivars->context->controller;
        auto stateStr = std::string(ToString(controller.StateMachine().CurrentState()));
        if (auto s = OSSharedPtr<OSString>(OSString::withCString(stateStr.c_str()), OSNoRetain)) {
            dict->setObject("state", s.get());
        }
        auto& m = controller.Metrics().BusReset();
        if (auto n = OSSharedPtr<OSNumber>(OSNumber::withNumber(m.resetCount, 32), OSNoRetain)) {
            dict->setObject("busResetCount", n.get());
        }
        if (auto n =
                OSSharedPtr<OSNumber>(OSNumber::withNumber(m.lastResetStart, 64), OSNoRetain)) {
            dict->setObject("lastResetStart", n.get());
        }
        if (auto n = OSSharedPtr<OSNumber>(OSNumber::withNumber(m.lastResetCompletion, 64),
                                           OSNoRetain)) {
            dict->setObject("lastResetCompletion", n.get());
        }
        if (!m.lastFailureReason.has_value()) {
            dict->removeObject("lastResetFailure");
        } else if (auto s = OSSharedPtr<OSString>(
                       OSString::withCString(m.lastFailureReason->c_str()), OSNoRetain)) {
            dict->setObject("lastResetFailure", s.get());
        }

        if (auto topo = controller.LatestTopology()) {
            if (auto n =
                    OSSharedPtr<OSNumber>(OSNumber::withNumber(topo->generation, 32), OSNoRetain)) {
                dict->setObject("topologyGeneration", n.get());
            }
            if (auto n = OSSharedPtr<OSNumber>(
                    OSNumber::withNumber(static_cast<uint64_t>(topo->physical.nodes.size()), 32),
                    OSNoRetain)) {
                dict->setObject("topologyNodeCount", n.get());
            }
        }
    }
    *status = dict;
    return kIOReturnSuccess;
}

// Positional out-parameters are part of the existing driver/user-client contract.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ASFWDriver::CopyControllerSnapshot(OSDictionary** status, uint64_t* sequence,
                                                 uint64_t* timestamp) {
    if (status) {
        auto kr = CopyControllerStatus(status);
        if (kr != kIOReturnSuccess) {
            return kr;
        }
    }

    if (sequence) {
        *sequence = 0;
    }
    if (timestamp) {
        *timestamp = 0;
    }

    if (!ivars || !ivars->context || !ivars->context->statusPublisher.StatusBlock()) {
        return kIOReturnSuccess;
    }

    const auto& block = *ivars->context->statusPublisher.StatusBlock();
    if (sequence) {
        *sequence = block.sequence;
    }
    if (timestamp) {
        *timestamp = block.updateTimestamp;
    }

    return kIOReturnSuccess;
}

void* ASFWDriver::GetControllerCore() const {
    if (!ivars || !ivars->context)
        return nullptr;
    return ivars->context->controller.get();
}

void* ASFWDriver::GetAsyncSubsystem() const {
    if (!ivars || !ivars->context)
        return nullptr;
    return ivars->context->deps.asyncController.get();
}

void* ASFWDriver::GetServiceContext() const {
    if (!ivars)
        return nullptr;
    return ivars->context;
}

kern_return_t IMPL(ASFWDriver, NewUserClient) {
    if (type != 0) {
        return kIOReturnBadArgument;
    }

    if (!userClient) {
        return kIOReturnBadArgument;
    }

    ASFW_LOG(Controller, "NewUserClient request received (type=%u)", type);

    IOService* userClientService = nullptr;
    auto ret = Create(this, "ASFWDriverUserClientProperties", &userClientService);
    if (ret != kIOReturnSuccess || !userClientService) {
        ASFW_LOG_ERROR(Controller, "NewUserClient Create failed: 0x%08x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoResources;
    }

    auto client = OSDynamicCast(ASFWDriverUserClient, userClientService);
    if (!client) {
        ASFW_LOG_ERROR(Controller, "NewUserClient cast failure");
        userClientService->release();
        return kIOReturnNoResources;
    }

    ASFW_LOG(UserClient, "[Lifecycle] NewUserClient manual Start client=%p provider=%p", client, this);
    ret = client->Start(this);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Controller, "NewUserClient Start failed: 0x%08x", ret);
        client->release();
        return ret;
    }

    *userClient = client;
    ASFW_LOG(Controller, "NewUserClient success (client=%p)", client);
    return kIOReturnSuccess;
}

namespace {
// Bring-up trace: the first interrupts show whether the line fires at all,
// and whether it keeps firing with nothing pending.
constexpr uint64_t kTracedInterrupts = 8;
// Interrupts with nothing of ours pending, within one second, that count as a
// storm on a shared line (b199 B760 report): the controller is masked and the
// machine stays up. A healthy shared line sees a few of these per second.
constexpr uint32_t kSpuriousInterruptStormLimit = 2000;
// After start complete the forced bus reset raises busReset and selfIDComplete
// at once (b202: 1 ms). No interrupt in this long means the line does not
// reach this driver (a vector routed elsewhere, a level line still owned by
// another driver early in the boot): the controller is masked and the runtime
// quiesced, the service stays claimed, the machine goes on. (b205: checking
// this inside Start cannot work, the handler runs on the queue Start holds.)
constexpr uint64_t kInterruptArrivalBudgetNs = 1'000'000'000ull;
} // namespace

void ASFWDriver::InterruptOccurred_Impl(ASFWDriver_InterruptOccurred_Args) {
    (void)action;

    // DIAGNOSTIC: Log every interrupt invocation
    ASFW_LOG_V3(Controller, "InterruptOccurred called: time=%llu count=%llu", time, count);

    if (!ivars || !ivars->context) {
        ASFW_LOG(Controller, "InterruptOccurred: no ivars or context");
        return;
    }
    auto& ctx = *ivars->context;
    if (!ctx.lifecycle || !ctx.lifecycle->AdmitsBringupInterrupts()) {
        return;
    }
    if (!ctx.controller || !ctx.deps.hardware) {
        ASFW_LOG(Controller, "InterruptOccurred: no controller or hardware");
        return;
    }
    // DriverKit delivers `time` in mach_absolute_time() ticks, NOT nanoseconds
    // (IOInterruptDispatchSource.iig: kIOInterruptSourceContinuousTime only swaps
    // mach_absolute→mach_continuous, never the unit; our source uses plain index 0).
    // Convert to ns at this single boundary so the value that flows into
    // snapshot.timestamp lands on the same scale as MonotonicNow(). Storing raw
    // ticks made every downstream "now(ns) − timestamp" ≈ uptime, which silently
    // defeated the IEEE 1394-2008 §8.2.1 two-second repeated-reset holdoff and
    // forced the Annex H post-reset timing gates permanently open.
    const uint64_t irqEntry = ctx.irqEntries.fetch_add(1, std::memory_order_relaxed) + 1;
    struct IrqExit {
        ServiceContext& context;
        ~IrqExit() { context.irqExits.fetch_add(1, std::memory_order_release); }
    } irqExit{ctx};
    const uint64_t timestampNs = ASFW::Timing::hostTicksToNanos(time);
    auto& hardware = *ctx.deps.hardware;
    auto* interrupts = ctx.deps.interrupts.get();
    bool irqTraced = false;
    bool firstPass = true;
    bool spurious = false;
    const uint32_t stillPending = ASFW::Driver::ServiceUntilDeasserted(
        [&] { return hardware.CaptureInterruptSnapshot(timestampNs); },
        [&] { return interrupts != nullptr ? interrupts->EnabledMask() : 0U; },
        [&](const ASFW::Driver::InterruptSnapshot& snap) {
            if (firstPass) {
                firstPass = false;
                spurious = interrupts != nullptr && (snap.intEvent & interrupts->EnabledMask()) == 0U;
            }
            // One line per interrupt (about 1700/s with two units streaming,
            // b178): trace only.
            ASFW_LOG_TRACE(Controller, "InterruptOccurred: captured snapshot intEvent=0x%08x",
                           snap.intEvent);
            if (irqEntry <= kTracedInterrupts && !irqTraced) {
                irqTraced = true;
                hardware.BringUpTrace().Step("interrupt %llu (count %llu): IntEvent 0x%08x",
                                             irqEntry, count, snap.intEvent);
            }
            ctx.interruptDispatcher.HandleSnapshot(snap, *ctx.controller, hardware,
                                                   ctx.isochLanes,
                                                   ctx.statusPublisher,
                                                   ctx.deps.asyncController.get());
        });
    if (spurious && interrupts != nullptr && !ctx.irqStormMasked) {
        if (timestampNs - ctx.irqSpuriousWindowNs > 1'000'000'000ull) {
            ctx.irqSpuriousWindowNs = timestampNs;
            ctx.irqSpurious = 0;
        }
        if (++ctx.irqSpurious >= kSpuriousInterruptStormLimit) {
            // Fail and continue: everything masked at the controller, the
            // machine stays up, the bus is down until the next start.
            ctx.irqStormMasked = true;
            interrupts->MaskInterrupts(&hardware, 0xFFFFFFFFu);
            ASFW_LOG_ERROR(Controller,
                           "[IrqStorm] %u interrupts with nothing of ours pending within 1 s; every event "
                           "masked (IntMaskClear all). The controller is deaf until the next start",
                           ctx.irqSpurious);
            hardware.BringUpTrace().Step("interrupt storm: %u with nothing pending within 1 s; masked",
                                         ctx.irqSpurious);
        }
    }
    if (stillPending == 0U) {
        ctx.irqConsecutiveRetriggers = 0;
    } else if (interrupts != nullptr) {
        if (++ctx.irqConsecutiveRetriggers <= ASFW::Driver::kMaxConsecutiveRetriggers) {
            // An event that will not acknowledge. Returning with it pending would
            // leave the driver deaf; make the controller send a fresh MSI instead.
            ASFW_LOG_ERROR(Controller,
                           "InterruptOccurred: events 0x%08x still pending after %d passes; "
                           "retriggering the interrupt",
                           stillPending, ASFW::Driver::kMaxInterruptPasses);
            interrupts->RetriggerPendingInterrupt(&hardware);
        } else {
            // Retriggering did not clear them either. Mask those events rather
            // than take MSIs forever; the rest of the controller keeps working.
            const uint32_t stuck = stillPending & ~ASFW::Driver::IntMaskBits::kMasterIntEnable;
            ASFW_LOG_ERROR(Controller,
                           "InterruptOccurred: events 0x%08x stuck after %u retriggers; masking them",
                           stuck, ASFW::Driver::kMaxConsecutiveRetriggers);
            interrupts->MaskInterrupts(&hardware, stuck);
            ctx.irqConsecutiveRetriggers = 0;
        }
    }
}

void ASFWDriver::ScheduleAsyncWatchdog(uint64_t delayUsec) {
    if (!ivars || !ivars->context) {
        return;
    }
    auto& ctx = *ivars->context;
    // Gate on the alive states (kStarting/kRunning), NOT AdmitsNormalWork():
    // the initial arm in StartRuntime happens before CompleteStart() flips the
    // state to kRunning, so a kRunning-only gate silently no-ops it and the
    // self-rearming tick chain never starts — no AT transaction timeout can
    // ever fire (0.3.0 regression, 22c82112; observed as an unrecoverable
    // SBP-2 wedge when a target stopped responding). Teardown still kills the
    // chain authoritatively via watchdog.Stop()/Reset() (timer disable), and
    // every return to kRunning re-enters StartRuntime, which re-arms.
    if (!ctx.lifecycle || !ctx.lifecycle->AdmitsBringupInterrupts()) {
        return;
    }
    ctx.watchdog.Schedule(delayUsec);
}

void ASFWDriver::AsyncWatchdogTimerFired_Impl(ASFWDriver_AsyncWatchdogTimerFired_Args) {
    (void)action;
    (void)time;

    if (ivars && ivars->context) {
        auto& ctx = *ivars->context;
        // Skip only the WORK when normal work is inadmissible — never the
        // reschedule below. An early return here breaks the self-rearming
        // chain permanently on a transient non-running state; the chain's
        // real kill switch is the timer disable in watchdog.Stop()/Reset().
        if (ctx.lifecycle && ctx.lifecycle->AdmitsNormalWork()) {
            if (!ctx.irqArrivalChecked && ctx.startCompleteNs != 0 &&
                ASFW::Timing::hostTicksToNanos(mach_absolute_time()) - ctx.startCompleteNs >=
                    kInterruptArrivalBudgetNs) {
                ctx.irqArrivalChecked = true;
                const uint64_t arrived = ctx.irqEntries.load(std::memory_order_acquire) - ctx.irqEntriesAtStart;
                if (arrived == 0) {
                    ASFW_LOG_ERROR(Controller,
                                   "❌ no interrupt within %llu ms of start complete, though the bus reset raised one: "
                                   "the line does not reach this driver. Masking the controller and quiescing the runtime",
                                   kInterruptArrivalBudgetNs / 1000000ull);
                    if (ctx.deps.hardware) {
                        ctx.deps.hardware->BringUpTrace().Step("no interrupt within %llu ms of start complete; quiescing",
                                                                kInterruptArrivalBudgetNs / 1000000ull);
                        if (ctx.deps.interrupts) {
                            ctx.deps.interrupts->MaskInterrupts(ctx.deps.hardware.get(), 0xFFFFFFFFu);
                        }
                    }
                    RequestRuntimeQuiesce(static_cast<uint32_t>(QuiesceReason::kSystemSuspend));
                    return;
                }
                ASFW_LOG(Controller, "interrupt arrival: %llu within %llu ms of start complete", arrived,
                         kInterruptArrivalBudgetNs / 1000000ull);
            }
            // Read-only anomaly probe. Linux ohci.c:2203-2225 samples IntEvent
            // before acknowledging it; this probe deliberately never clears it.
            // 100 ms sampling avoids MMIO/logging on each watchdog or IRQ tick.
            if (++ctx.irqProbeTicks >= 100 && ctx.deps.hardware && ctx.deps.interrupts) {
                ctx.irqProbeTicks = 0;
                auto access = ctx.deps.hardware->TryBeginAccess();
                if (access) {
                    using namespace ASFW::Driver;
                    const auto events = access.Read(Register32::kIntEventSet);
                    const auto mask = access.Read(Register32::kIntMaskSet);
                    constexpr uint32_t relevant = IntEventBits::kReqTxComplete | IntEventBits::kRespTxComplete |
                        IntEventBits::kARRQ | IntEventBits::kARRS | IntEventBits::kRQPkt | IntEventBits::kRSPkt |
                        IntEventBits::kBusReset | IntEventBits::kSelfIDComplete;
                    const auto pending = events & mask & relevant;
                    const auto entries = ctx.irqEntries.load(std::memory_order_acquire);
                    const auto exits = ctx.irqExits.load(std::memory_order_acquire);
                    if (events != UINT32_MAX && pending && entries == ctx.irqProbeLastEntries) {
                        ++ctx.irqPendingSamples;
                        if (ctx.irqPendingSamples >= 5 && ctx.irqProbeReports < 3) {
                            ++ctx.irqProbeReports;
                            ASFW_LOG_ERROR(Controller,
                                "[IrqStall] pending=0x%08x events=0x%08x mask=0x%08x shadow=0x%08x entries=%llu exits=%llu samples=%u node=0x%08x atReq=0x%08x atRsp=0x%08x arReq=0x%08x arRsp=0x%08x",
                                pending, events, mask, ctx.deps.interrupts->EnabledMask(), entries, exits,
                                ctx.irqPendingSamples, access.Read(Register32::kNodeID),
                                access.Read(static_cast<Register32>(DMAContextHelpers::AsReqTrContextControlSet)),
                                access.Read(static_cast<Register32>(DMAContextHelpers::AsRspTrContextControlSet)),
                                access.Read(static_cast<Register32>(DMAContextHelpers::AsReqRcvContextControlSet)),
                                access.Read(static_cast<Register32>(DMAContextHelpers::AsRspRcvContextControlSet)));
                        }
                    } else {
                        ctx.irqPendingSamples = 0;
                        ctx.irqProbeReports = 0;
                    }
                    ctx.irqProbeLastEntries = entries;
                }
            }
            ctx.watchdog.HandleTick(ctx.controller.get(), ctx.deps.asyncController.get(),
                                    ctx.isoch.ReceiveContext(), ctx.isoch.TransmitContext(),
                                    ctx.statusPublisher);
        }
    }

    ScheduleAsyncWatchdog(ivars && ivars->context
                              ? WatchdogCoordinator::TickPeriodUsec(ivars->context->isochLanes)
                              : kAsyncWatchdogPeriodUsec);
}

void ASFWDriver::SBP2SessionTimerFired_Impl(ASFWDriver_SBP2SessionTimerFired_Args) {
    (void)action;
    (void)time;

    if (!ivars || !ivars->context) {
        return;
    }
    auto& ctx = *ivars->context;
    if (!ctx.lifecycle || !ctx.lifecycle->AdmitsNormalWork() || !ctx.deps.sbp2SessionScheduler) {
        return;
    }

    ctx.deps.sbp2SessionScheduler->HandleTimerFired();
}

void ASFWDriver::ProviderNotificationReady_Impl(ASFWDriver_ProviderNotificationReady_Args) {
    (void)action;

    if (!ivars || !ivars->context) {
        return;
    }
    auto& ctx = *ivars->context;

#ifndef ASFW_HOST_TEST
    if (!ctx.providerNotifications) {
        return;
    }

    __block bool providerTerminated = false;
    (void)ctx.providerNotifications->DeliverNotifications(
        ^(uint64_t type, IOService* service, uint64_t options) {
          (void)service;
          (void)options;
          if (type == kIOServiceNotificationTypeTerminated) {
              providerTerminated = true;
          }
        });

    if (!providerTerminated) {
        return;
    }

    if (ctx.deps.hardware) {
        ctx.deps.hardware->LatchProviderRevokedAndDrain();
    }
    RequestRuntimeQuiesce(static_cast<uint32_t>(QuiesceReason::kProviderRevoked));
#endif
}

kern_return_t ASFWDriver::CopySharedStatusMemory(uint64_t* options,
                                                 IOMemoryDescriptor** memory) const {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }

    return ivars->context->statusPublisher.CopySharedMemory(options, memory);
}

kern_return_t ASFWDriver::StartAudioStreaming(uint64_t guid) {
    if (!ivars || !ivars->context || !ivars->context->audioCoordinator) {
        ASFW_LOG_ERROR(Audio,
                       "[BeBoB] developer stream start refused stage=%{public}s GUID=0x%016llx",
                       "audio-coordinator", guid);
        return kIOReturnNotReady;
    }
    auto& ctx = *ivars->context;
    // The normal AudioDriverKit path refreshes FCP routing immediately before
    // each AV/C start. MCP must do the same: a GUID survives a bus reset while
    // its node/FCP transport does not. This prevents a developer start from
    // issuing the PHASE 88's unit-plug format command through a stale route.
    if (!ctx.deps.deviceRegistry || !ctx.deps.audioRuntimeRegistry ||
        !ctx.deps.avcDiscovery) {
        ASFW_LOG_ERROR(Audio,
                       "[BeBoB] developer stream start refused stage=%{public}s GUID=0x%016llx",
                       "runtime-dependencies", guid);
        return kIOReturnNotReady;
    }
    const auto record = ctx.deps.deviceRegistry->SnapshotByGuid(guid);
    auto protocol = ctx.deps.audioRuntimeRegistry->FindShared(guid);
    if (!record.has_value() || !protocol) {
        ASFW_LOG_ERROR(Audio,
                       "[BeBoB] developer stream start refused stage=%{public}s GUID=0x%016llx record=%u protocol=%u",
                       "device-config", guid, record.has_value(), protocol != nullptr);
        return kIOReturnNotReady;
    }
    auto avcUnit = ctx.deps.avcDiscovery->LiveUnit(guid);
    if (!ASFW::Audio::HasReadyAVCStartRoute(record->nodeId, avcUnit != nullptr)) {
        ASFW_LOG_WARNING(Audio,
                         "[BeBoB] developer stream start refused; no live FCP route GUID=0x%016llx node=%u",
                         guid, record->nodeId);
        return kIOReturnNotReady;
    }
    const auto route = ctx.deps.deviceRegistry->CurrentRoute(guid);
    if (!route) {
        return kIOReturnNotReady;
    }
    protocol->UpdateRuntimeContext(*route, std::move(avcUnit));
    ASFW_LOG(Audio, "[BeBoB] developer stream start GUID=0x%016llx", guid);
    return ctx.audioCoordinator->StartStreaming(guid);
}

kern_return_t ASFWDriver::StopAudioStreaming(uint64_t guid) {
    if (!ivars || !ivars->context || !ivars->context->audioCoordinator) {
        return kIOReturnNotReady;
    }
    ASFW_LOG(Audio, "[BeBoB] developer stream stop GUID=0x%016llx", guid);
    return ivars->context->audioCoordinator->StopStreaming(guid);
}

// =============================================================================
// MARK: - DV Capture (no audio nub required)
// =============================================================================

kern_return_t ASFWDriver::StartDVCapture(uint64_t deviceGuid,
                                         uint64_t ownerToken) {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }
    auto& ctx = *ivars->context;
    if (!ctx.deps.hardware) {
        ASFW_LOG_ERROR(Controller, "[Isoch] ❌ StartDVCapture: hardware not ready");
        return kIOReturnNotReady;
    }
    // dev2 replaced the ServiceContext::stopping flag with the lifecycle
    // coordinator's state machine; only a fully running runtime may start capture.
    if (!ctx.lifecycle || ctx.lifecycle->CurrentState() != ControllerState::kRunning) {
        return kIOReturnOffline;
    }
    if (!ctx.deps.deviceRegistry || !ctx.deps.irmClient ||
        !ctx.deps.cmpClient) {
        return kIOReturnNotReady;
    }
    return ctx.dvCapture.Start(deviceGuid, ownerToken, ctx.isoch,
                               *ctx.deps.hardware, *ctx.deps.deviceRegistry,
                               *ctx.deps.irmClient, *ctx.deps.cmpClient);
}

kern_return_t ASFWDriver::StopDVCapture(uint64_t ownerToken) {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }
    return ivars->context->dvCapture.Stop(ownerToken,
                                          ivars->context->isoch);
}

kern_return_t ASFWDriver::CopyDVCaptureMemory(
    uint64_t ownerToken,
    uint64_t* options,
    IOMemoryDescriptor** memory) const {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }
    return ivars->context->dvCapture.CopyMemory(ownerToken, options, memory);
}

kern_return_t ASFWDriver::CopyAudioObserverMemory(
    uint64_t guid,
    uint64_t* outMemoryGeneration,
    IOMemoryDescriptor** outMemory) {
    if (outMemoryGeneration) { *outMemoryGeneration = 0; }
    if (outMemory) { *outMemory = nullptr; }
    if (guid == 0 || !outMemoryGeneration || !outMemory) {
        return kIOReturnBadArgument;
    }
    if (!ivars || !ivars->context || !ivars->context->deps.audioRuntimeRegistry) {
        return kIOReturnNotReady;
    }
    auto endpoint = ivars->context->deps.audioRuntimeRegistry->FindEndpointRuntime(guid);
    if (!endpoint) {
        return kIOReturnNotFound;
    }
    ASFW::Audio::AudioOutputObserverState state{};
    const kern_return_t kr = endpoint->CopyOutputObserverMemory(outMemory, state);
    if (kr == kIOReturnSuccess) {
        *outMemoryGeneration = state.memoryGeneration;
    }
    return kr;
}

kern_return_t ASFWDriver::CopyAudioObserverState(
    uint64_t guid,
    uint64_t* outWriteEndFrame,
    uint64_t* outOldestValidFrame,
    uint64_t* outSessionEpoch,
    uint64_t* outDiscontinuityEpoch,
    uint64_t* outMemoryGeneration,
    uint32_t* outActiveRingFrames,
    uint32_t* outChannels,
    uint32_t* outSampleRateHz) {
    if (outWriteEndFrame) { *outWriteEndFrame = 0; }
    if (outOldestValidFrame) { *outOldestValidFrame = 0; }
    if (outSessionEpoch) { *outSessionEpoch = 0; }
    if (outDiscontinuityEpoch) { *outDiscontinuityEpoch = 0; }
    if (outMemoryGeneration) { *outMemoryGeneration = 0; }
    if (outActiveRingFrames) { *outActiveRingFrames = 0; }
    if (outChannels) { *outChannels = 0; }
    if (outSampleRateHz) { *outSampleRateHz = 0; }
    if (guid == 0 || !outWriteEndFrame || !outOldestValidFrame ||
        !outSessionEpoch || !outDiscontinuityEpoch || !outMemoryGeneration ||
        !outActiveRingFrames || !outChannels || !outSampleRateHz) {
        return kIOReturnBadArgument;
    }
    if (!ivars || !ivars->context || !ivars->context->deps.audioRuntimeRegistry) {
        return kIOReturnNotReady;
    }
    auto endpoint = ivars->context->deps.audioRuntimeRegistry->FindEndpointRuntime(guid);
    if (!endpoint) {
        return kIOReturnNotFound;
    }
    ASFW::Audio::AudioOutputObserverState state{};
    const kern_return_t kr = endpoint->CopyOutputObserverState(state);
    if (kr != kIOReturnSuccess) {
        return kr;
    }
    *outWriteEndFrame = state.writeEndFrame;
    *outOldestValidFrame = state.oldestValidFrame;
    *outSessionEpoch = state.sessionEpoch;
    *outDiscontinuityEpoch = state.discontinuityEpoch;
    *outMemoryGeneration = state.memoryGeneration;
    *outActiveRingFrames = state.activeRingFrames;
    *outChannels = state.channels;
    *outSampleRateHz = state.sampleRateHz;
    return kIOReturnSuccess;
}

// =============================================================================
// MARK: - Isochronous Transmit
// =============================================================================

kern_return_t ASFWDriver::StartIsochTransmit(uint8_t channel) {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }
    auto& ctx = *ivars->context;
    if (!ctx.deps.asyncSubsystem || !ctx.deps.hardware) {
        ASFW_LOG_ERROR(Controller, "[Isoch] ❌ StartIsochTransmit: Subsystems not ready");
        return kIOReturnNotReady;
    }

    const uint8_t sid = static_cast<uint8_t>(ctx.deps.hardware->ReadNodeID() & 0x3Fu);

    // A user-client diagnostic path with no device behind it, so there is no
    // per-device link speed to follow. S400 is the same speed this path has
    // always transmitted at; the audio path derives its speed from the target
    // device and passes it explicitly.
    return ctx.isoch.StartTransmit(channel, *ctx.deps.hardware, sid, ASFW::FW::FwSpeed::S400);
}

kern_return_t ASFWDriver::StopIsochTransmit() {
    if (!ivars || !ivars->context || !ivars->context->isoch.TransmitContext()) {
        return kIOReturnNotReady;
    }
    return ivars->context->isoch.StopTransmit();
}

void* ASFWDriver::GetIsochTransmitContext() const {
    if (!ivars || !ivars->context) {
        return nullptr;
    }
    return ivars->context->isoch.TransmitContext();
}
