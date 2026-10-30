#include "ASFWAvcAudioStream.h"
//
// ASFWAudioDriverTeardown.cpp
// ASFWDriver
//
// ADK graph teardown for ASFWAudioDriver, shared by a failed Start and by Stop.
// Kept apart from graph construction so the host suite can check that a
// teardown leaves the driver holding no ADK object.
//
#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Audio::DriverKit {

void UnbindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars) noexcept {
    ivars.runtime.directAudioSkeletonBound.store(false, std::memory_order_release);
    ivars.runtime.directAudioGraph = {};
    ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);
}

void TearDownAudioGraph(ASFWAudioDriver& driver, ASFWAudioDriver_IVars& ivars) noexcept {
    ivars.runtime.isRunning.store(false, std::memory_order_release);
    UnbindDirectAudioSkeleton(ivars);

    if (auto* stream = OSDynamicCast(ASFWAvcAudioStream, ivars.inputStream.get())) stream->Bind(nullptr);
    if (auto* stream = OSDynamicCast(ASFWAvcAudioStream, ivars.outputStream.get())) stream->Bind(nullptr);
    auto& state = ivars.graphState;
    // The streams and the device stay registered with AudioDriverKit: its own
    // Stop (IOUserAudioDriver::Stop_Impl, which ours calls next) disconnects
    // CoreAudio's user client, stops and unregisters the device, and calls
    // _DriverServiceStopped on every object, which drops each object's
    // reference to this driver (DK21 and DK25 disassembly). Removing them here
    // deadlocked when CoreAudio's user client went at the same time (b178
    // hotplug): RemoveStream takes the device's queue and then, to tell
    // CoreAudio, the user client's lock; the client's teardown holds that lock,
    // then the driver's work queue, then waits for the device's queue.
    (void)driver;
    if (ivars.audioDevice) {
        // ADK may keep the device past this driver; it must not reach these
        // ivars after they are gone.
        ivars.audioDevice->SetDriverIvars(nullptr);
    }
    state = {};

    ivars.device.avcControlCount = 0;
    ivars.device.boolControlCount = 0;
    ivars.device.clockSourceCount = 0;
    ivars.clockSourceControl.reset();
    ASFW::Isoch::Audio::ResetBoolControlSlots(ivars.device.boolControls,
                                              ASFW::Isoch::Audio::kMaxBoolControls);
    ivars.outputStream.reset();
    ivars.inputStream.reset();
    ivars.outputMap.reset();
    ivars.inputMap.reset();
    ivars.controlMap.reset();
    ivars.outputBuffer.reset();
    ivars.inputBuffer.reset();
    ivars.controlBuffer.reset();
    ivars.audioDevice.reset();
    ivars.workQueue.reset();
    ivars.device.audioNub = nullptr;
}

void StopAudioDriverGraph(ASFWAudioDriver& driver, ASFWAudioDriver_IVars& ivars) noexcept {
    // IO still running: the device left mid-stream. Stop it here, while the
    // device is still attached. CoreAudio's own StopIO arrives after the
    // device is removed below and no longer reaches it, so ADK's IO loop kept
    // cycling with no timestamps (62% CPU for minutes) and neither the driver
    // nor the nub was freed (Pro 24 DSP unplug while playing, 2026-10-09).
    // super::StopDevice stops ADK's IO state and calls our StopIO.
    if (ivars.audioDevice && ivars.runtime.isRunning.load(std::memory_order_acquire)) {
        const uint32_t deviceId = ivars.audioDevice->GetObjectID();
        ASFW_LOG(Audio, "ASFWAudioDriver: Stop() with IO running; stopping device %u first",
                 deviceId);
        (void)driver.StopDevice(deviceId, IOUserAudioStartStopFlags::None);
    }
    // A wire kept across StopIO (including by the stop just above) gets no
    // StopIO on unplug: release it the way StopIO does, or the TX isoch
    // resources and their mappings outlive the device (Phase 88 unplug while
    // retained, 2026-10-08: no release, no free()).
    if (ivars.audioDevice) {
        (void)ivars.audioDevice->ReleaseKeptWireForDriverStop();
    }
    ivars.runtime.isRunning.store(false, std::memory_order_release);
    // A wire kept across StopIO (Runtime/WireRetention.hpp) still has its TX
    // producer armed: stop it and wait out a pass in flight before the stream
    // stops and the slabs go (TX_OWNERSHIP.md T6). Harmless when IO already
    // stopped it.
    ivars.runtime.txActive.store(false, std::memory_order_release);
    if (ivars.txPreparationQueue) {
        ivars.txPreparationQueue->DispatchSync(^{ });
    }
    if (auto* nub = ivars.device.audioNub) {
        const kern_return_t stopKr = nub->StopAudioStreamingOrRemoteResult();
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio, "ASFWAudioDriver: StopAudioStreaming failed in Stop(): 0x%x (%{public}s)", stopKr, ASFW::Logging::IOReturnName(stopKr));
        }
        (void)nub->RegisterTxPreparationAction(nullptr);
        (void)nub->RegisterZtsAnchorAction(nullptr);
        (void)nub->RegisterDeviceClockChangedAction(nullptr);
        (void)nub->RegisterIoRestartRequiredAction(nullptr);
    }
    ivars.txPreparationAction.reset();
    ivars.txPreparationQueue.reset();
    ivars.deviceClockChangedAction.reset();
    ivars.ioRestartRequiredAction.reset();
    ivars.ztsAnchorAction.reset();
    ivars.ztsQueue.reset();
    // Drop the whole ADK graph here, not in free(). The device is created with
    // this driver (ASFWAudioDevice::init(&driver) retains it until ADK's Stop),
    // so a device still held by the ivars can keep the driver from ever being
    // freed, and with it the dext process alive after an unplug (2026-10-02).
    TearDownAudioGraph(driver, ivars);
}

} // namespace ASFW::Audio::DriverKit
