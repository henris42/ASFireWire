// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioCoordinator.hpp"

#include "AudioEndpointRuntime.hpp"
#include "AudioRuntimeRegistry.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../Protocols/DeviceProtocolChoice.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include <net.asfw.driver/ASFWAudioNub.h>
#include <cstdio>
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"

namespace ASFW::Audio {

// Forwards to the lane the GUID holds when the call is made.
class AudioCoordinator::LaneTransport final : public IIsochDuplexHostTransport {
  public:
    LaneTransport(AudioCoordinator& owner, uint64_t guid) noexcept : owner_(owner), guid_(guid) {}

    [[nodiscard]] kern_return_t BeginSplitDuplex(uint64_t guid) noexcept override {
        return Lane().BeginSplitDuplex(guid);
    }
    [[nodiscard]] kern_return_t
    ReservePlaybackResources(uint64_t guid, ::ASFW::IRM::IRMClient& irmClient, uint64_t allowedChannels,
                             uint32_t packetBandwidthUnits,
                             Backends::IRMReservationResult& outResult) noexcept override {
        return Lane().ReservePlaybackResources(guid, irmClient, allowedChannels, packetBandwidthUnits, outResult);
    }
    [[nodiscard]] kern_return_t
    ReserveCaptureResources(uint64_t guid, ::ASFW::IRM::IRMClient& irmClient, uint64_t allowedChannels,
                            uint32_t packetBandwidthUnits,
                            Backends::IRMReservationResult& outResult) noexcept override {
        return Lane().ReserveCaptureResources(guid, irmClient, allowedChannels, packetBandwidthUnits, outResult);
    }
    [[nodiscard]] kern_return_t PrepareReceive(uint8_t channel, Driver::HardwareInterface& hardware,
                                               Runtime::IDirectAudioBindingSource* bindingSource,
                                               const DirectRxFormatDescriptor& format) noexcept override {
        return Lane().PrepareReceive(channel, hardware, bindingSource, format);
    }
    [[nodiscard]] kern_return_t PrepareTransmit(uint8_t channel, Driver::HardwareInterface& hardware,
                                                uint8_t sourceId, FW::FwSpeed speed) noexcept override {
        return Lane().PrepareTransmit(channel, hardware, sourceId, speed);
    }
    [[nodiscard]] kern_return_t PrepareReceiveStream(uint32_t streamIndex, uint8_t channel,
                                                     Driver::HardwareInterface& hardware,
                                                     Runtime::IDirectAudioBindingSource* bindingSource,
                                                     uint32_t channelOffset,
                                                     const DirectRxFormatDescriptor& format) noexcept override {
        return Lane().PrepareReceiveStream(streamIndex, channel, hardware, bindingSource, channelOffset, format);
    }
    [[nodiscard]] kern_return_t PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                                      Driver::HardwareInterface& hardware, uint8_t sourceId,
                                                      FW::FwSpeed speed) noexcept override {
        return Lane().PrepareTransmitStream(streamIndex, channel, hardware, sourceId, speed);
    }
    [[nodiscard]] kern_return_t StartPreparedReceive() noexcept override { return Lane().StartPreparedReceive(); }
    [[nodiscard]] kern_return_t StartPreparedTransmit() noexcept override { return Lane().StartPreparedTransmit(); }
    [[nodiscard]] kern_return_t StopPreparedReceive() noexcept override { return Lane().StopPreparedReceive(); }
    [[nodiscard]] kern_return_t StopPreparedTransmit() noexcept override { return Lane().StopPreparedTransmit(); }
    [[nodiscard]] kern_return_t StopAll() noexcept override { return Lane().StopAll(); }
    [[nodiscard]] kern_return_t StopAllAfterBusReset() noexcept override { return Lane().StopAllAfterBusReset(); }
    [[nodiscard]] bool IsReceiveReplayEstablished() const noexcept override {
        return owner_.TransportForGuid(guid_).IsReceiveReplayEstablished();
    }

  private:
    [[nodiscard]] IIsochDuplexHostTransport& Lane() noexcept { return owner_.TransportForGuid(guid_); }
    AudioCoordinator& owner_;
    uint64_t guid_;
};

AudioCoordinator::AudioCoordinator(IOService* driver,
                                   Discovery::IDeviceManager& deviceManager,
                                   Discovery::DeviceRegistry& registry,
                                   AudioRuntimeRegistry& runtime,
                                    Driver::IsochLanes& lanes,
                                    Driver::HardwareInterface& hardware,
                                    DICE::DiceNotificationRouter& diceNotifications) noexcept
    : publisher_(driver)
    , deviceManager_(deviceManager)
    , registry_(registry)
    , runtime_(runtime)
    , lanes_(lanes)
    , hardware_(hardware)
    , sessions_(registry_, runtime_,
                [this](uint64_t guid) -> IIsochDuplexHostTransport& { return SessionTransportForGuid(guid); },
                hardware, &teardownRequested_,
                [this](uint64_t guid) -> Runtime::IDirectAudioBindingSource* {
                    auto endpoint = runtime_.FindEndpointRuntime(guid);
                    return endpoint ? endpoint.get() : nullptr;
                })
    , diceAdapter_(diceNotifications)
    , host_(publisher_, registry_, runtime_, sessions_,
            [this](uint64_t guid) -> IIsochDuplexHostTransport& { return SessionTransportForGuid(guid); }) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AudioCoordinator: Failed to allocate lock");
    }
    // Lane 0 is the controller's original service; it exists from the start.
    if (auto* lane0 = lanes_.Lane(0)) {
        transports_[0] = std::make_unique<IsochDuplexHostTransport>(*lane0);
        InstallLaneCallbacks(0);
    }

    host_.Install(AudioBackendKind::Dice, diceAdapter_);
    host_.Install(AudioBackendKind::RmeRegister, rmeAdapter_);
    host_.Install(AudioBackendKind::Avc, avcAdapter_);
    host_.Install(AudioBackendKind::MotuRegister, motuAdapter_);

    sessions_.SetStartGuard([this](uint64_t guid) {
        return !publisher_.IsGeometryChangeBlocked(guid);
    });
    // A restart while CoreAudio runs the streams goes through the host
    // (StopIO -> StartIO) so the audio-owned TX queue is rebuilt too.
    sessions_.SetHostRestartRouter([this](uint64_t guid, DuplexRestartReason reason) {
        ASFWAudioNub* nub = publisher_.GetNub(guid);
        return nub != nullptr && nub->NotifyIoRestartRequired(static_cast<uint32_t>(reason));
    });
    sessions_.SetRestartObserver([this](uint64_t guid) {
        if (ServedByHost(guid)) host_.RefreshPublication(guid);
    });
    deviceManager_.RegisterDeviceObserver(this);
    ASFW_LOG(Audio, "AudioCoordinator: Registered device observer");
}

AudioCoordinator::~AudioCoordinator() noexcept {
    deviceManager_.UnregisterDeviceObserver(this);
    for (uint32_t lane = 0; lane < transports_.size(); ++lane) {
        if (!transports_[lane]) continue;
        transports_[lane]->SetTimingLossCallback({});
        if (auto* service = lanes_.Lane(lane)) {
            service->SetTxPreparationCallback({});
            service->SetZtsAnchorReadyCallback({});
        }
    }

    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void AudioCoordinator::SetCMPClient(ASFW::CMP::CMPClient* client) noexcept {
    runtime_.SetCMPClient(client);
}

void AudioCoordinator::OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) return;
    const uint64_t guid = device->GetGUID();
    sessions_.Present(guid);
    if (lock_) {
        IOLockLock(lock_);
        remoteLostStopResults_.erase(guid);
        IOLockUnlock(lock_);
    }
    if (ServedByHost(guid)) host_.RefreshPublication(guid);
}

void AudioCoordinator::OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) return;
    const uint64_t guid = device->GetGUID();
    sessions_.Present(guid);
    if (lock_) {
        IOLockLock(lock_);
        remoteLostStopResults_.erase(guid);
        IOLockUnlock(lock_);
    }
    const bool hostServed = ServedByHost(guid);
    if (hostServed) host_.RefreshPublication(guid);

    const bool recoverActiveStream = sessions_.IsStreaming(guid);
    if (!recoverActiveStream) {
        return;
    }

    if (hostServed) host_.OnDeviceResumed(guid);
}

uint64_t AudioCoordinator::ActiveGuid() const noexcept {
    if (!lock_) return 0;
    IOLockLock(lock_);
    uint64_t guid = 0;
    for (const uint64_t owner : laneOwner_) {
        if (owner != 0) { guid = owner; break; }
    }
    IOLockUnlock(lock_);
    return guid;
}

uint64_t AudioCoordinator::GuidForLane(uint32_t lane) const noexcept {
    if (!lock_ || lane >= laneOwner_.size()) return 0;
    IOLockLock(lock_);
    const uint64_t guid = laneOwner_[lane];
    IOLockUnlock(lock_);
    return guid;
}

Driver::IsochService* AudioCoordinator::IsochServiceForGuid(uint64_t guid) noexcept {
    if (!lock_ || guid == 0) return nullptr;
    IOLockLock(lock_);
    const uint32_t lane = LaneOfLocked(guid);
    IOLockUnlock(lock_);
    return lane == kAllLanes ? nullptr : lanes_.Lane(lane);
}

uint32_t AudioCoordinator::LaneOfLocked(uint64_t guid) const noexcept {
    for (uint32_t lane = 0; lane < laneOwner_.size(); ++lane) {
        if (laneOwner_[lane] == guid) return lane;
    }
    return kAllLanes;
}

uint32_t AudioCoordinator::ClaimLaneLocked(uint64_t guid) noexcept {
    const uint32_t current = LaneOfLocked(guid);
    if (current != kAllLanes) return current;
    const uint32_t count = lanes_.LaneCount(hardware_);
    uint32_t lane = kAllLanes;
    // The lowest free lane, not the one the device used last: a unit that
    // once landed on a high lane must not stay there when it plays alone
    // (three 828mkIIs, 2026-10-28: the third unit kept lane 2 by itself).
    for (uint32_t candidate = 0; lane == kAllLanes && candidate < count && candidate < laneOwner_.size();
         ++candidate) {
        if (laneOwner_[candidate] == 0) lane = candidate;
    }
    if (lane == kAllLanes) return kAllLanes;
    Driver::IsochService* service = lanes_.EnsureLane(lane, hardware_);
    if (service == nullptr) return kAllLanes;
    if (!transports_[lane]) {
        transports_[lane] = std::make_unique<IsochDuplexHostTransport>(*service);
        InstallLaneCallbacks(lane);
    }
    laneOwner_[lane] = guid;
    laneByGuid_[guid] = lane;
    return lane;
}

void AudioCoordinator::ReleaseLaneLocked(uint64_t guid) noexcept {
    for (auto& owner : laneOwner_) {
        if (owner == guid) owner = 0;
    }
}

IsochDuplexHostTransport* AudioCoordinator::TransportForLane(uint32_t lane) noexcept {
    return lane < transports_.size() ? transports_[lane].get() : nullptr;
}

IIsochDuplexHostTransport& AudioCoordinator::TransportForGuid(uint64_t guid) noexcept {
    uint32_t lane = kAllLanes;
    if (lock_ && guid != 0) {
        IOLockLock(lock_);
        lane = LaneOfLocked(guid);
        if (lane == kAllLanes) {
            if (auto it = laneByGuid_.find(guid); it != laneByGuid_.end()) lane = it->second;
        }
        IOLockUnlock(lock_);
    }
    if (auto* transport = TransportForLane(lane)) return *transport;
    return *transports_[0];
}

IIsochDuplexHostTransport& AudioCoordinator::SessionTransportForGuid(uint64_t guid) noexcept {
    if (!lock_) return *transports_[0];
    IOLockLock(lock_);
    auto& slot = sessionTransports_[guid];
    if (!slot) slot = std::make_unique<LaneTransport>(*this, guid);
    LaneTransport& transport = *slot;
    IOLockUnlock(lock_);
    return transport;
}

void AudioCoordinator::InstallLaneCallbacks(uint32_t lane) noexcept {
    auto* transport = TransportForLane(lane);
    auto* service = lanes_.Lane(lane);
    if (!transport || !service) return;
    transport->SetTimingLossCallback([this](uint64_t guid) { HandleHostTimingLoss(guid); });
    transport->SetCancelToken(&teardownRequested_);
    // The service wakes the producer of the device streaming on this lane;
    // resolved at call time so a nub that registered later never takes
    // another lane's wakes (two 828mkIIs, b113).
    service->SetTxPreparationCallback(
        [this, lane](uint64_t generation) { WakeLaneOwner(lane, generation, /*ztsAnchor=*/false); });
    service->SetZtsAnchorReadyCallback(
        [this, lane](uint64_t generation) { WakeLaneOwner(lane, generation, /*ztsAnchor=*/true); });
}

void AudioCoordinator::WakeLaneOwner(uint32_t lane, uint64_t generation, bool ztsAnchor) noexcept {
    const uint64_t guid = GuidForLane(lane);
    if (guid == 0) return;
    ASFWAudioNub* nub = publisher_.GetNub(guid);
    if (!nub) return;
    if (ztsAnchor) {
        nub->WakeZtsAnchor(generation);
    } else {
        nub->WakeTxPreparation(generation);
    }
}

bool AudioCoordinator::HasActiveSession() const noexcept {
    return ActiveGuid() != 0;
}

void AudioCoordinator::HandleBusReset() noexcept {
    std::array<uint64_t, Driver::IsochLanes::kMaxLanes> owners{};
    if (lock_) {
        IOLockLock(lock_);
        owners = laneOwner_;
        IOLockUnlock(lock_);
    }
    if (teardownRequested_.load(std::memory_order_acquire)) return;
    for (uint32_t lane = 0; lane < owners.size(); ++lane) {
        const uint64_t guid = owners[lane];
        if (guid == 0) continue;
        sessions_.CancelPendingRestart(guid);
    // Linux dice-stream.c:587-605 stops the domain on reset because firmware
    // loses stream synchronization. Never spend the ROM-scan interval running
    // the old finite IT mapping. The generation invalidated its IRM leases;
    // local cleanup must not issue remote release/device-stop transactions.
    // Do not destroy consumers or mutate the session's reservation ledger on
    // this interrupt queue while reconciliation may still own them.
        auto* transport = TransportForLane(lane);
        const auto status = transport ? transport->QuiesceForBusReset() : kIOReturnNotReady;
        ASFW_LOG(Audio, "[StopTrace] owner=reset guid=%016llx lane=%u local=0x%x (%{public}s) action=await-rebind",
                 guid, lane, status, ASFW::Logging::IOReturnName(status));
    }
}

void AudioCoordinator::OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) {
        return;
    }

    const uint64_t guid = device->GetGUID();
    // A restart still waiting out its quiet period targets the generation that
    // just ended; the resume requests a fresh one. Firing it now would restart
    // a device with no operational node (hardware, 2026-09-25: a reset landing
    // as the quiet period expired sent CoreAudio a restart it could not start).
    sessions_.CancelPendingRestart(guid);
    const bool suspendedActiveStream = sessions_.IsStreaming(guid);
    if (!suspendedActiveStream) {
        return;
    }

    ASFW_LOG_WARNING(Audio,
                     "AudioCoordinator: Active device suspended; waiting for resume to recover GUID=0x%016llx",
                     guid);
}

void AudioCoordinator::OnDeviceRemoved(Discovery::Guid64 guid) {
    if (guid == 0) {
        return;
    }

    bool wasActive = false;
    uint32_t lane = kAllLanes;
    bool firstRemoval = true;
    if (lock_) {
        IOLockLock(lock_);
        firstRemoval = remoteLostStopResults_.try_emplace(guid, kIOReturnNotReady).second;
        lane = LaneOfLocked(guid);
        wasActive = lane != kAllLanes;
        if (wasActive) ReleaseLaneLocked(guid);
        IOLockUnlock(lock_);
    }
    if (!firstRemoval) {
        return;
    }

    // Discovery has completed a new-generation scan and confirmed this GUID is
    // absent. Latch before touching host work: delayed recovery and StopIO
    // callbacks must not recreate a session for the old route.
    sessions_.Retire(guid);
    // The registry has already invalidated the route policy by the time
    // removal is reported. Cancel the host's work for it so cleanup does not depend
    // on resolving a policy for a device that is known to be gone.
    host_.CancelRemoteDeviceWork(guid);

    kern_return_t hostStatus = kIOReturnSuccess;
    if (wasActive) {
        // Do not release IRM resources after the reset that proved the remote
        // device absent: that allocation is already invalid in this generation.
        hostStatus = StopHostTransport("remote-device-lost", true, lane);
        if (hostStatus != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio,
                           "AudioCoordinator: remote-device host teardown incomplete "
                           "GUID=0x%016llx kr=0x%08x (%{public}s); completing removal",
                           guid, hostStatus, ASFW::Logging::IOReturnName(hostStatus));
        }
    }

    // Drop cross-seam transport views before unpublishing CoreAudio. Runtime
    // shared_ptr copies keep an already executing control operation alive, but
    // the terminal latch prevents it from starting a new duplex session.
    if (lock_) {
        IOLockLock(lock_);
        if (auto it = remoteLostStopResults_.find(guid); it != remoteLostStopResults_.end())
            it->second = hostStatus;
        IOLockUnlock(lock_);
    }
    runtime_.Remove(guid);
    publisher_.TerminateNub(guid, "remote-device-lost", hostStatus);
    sessions_.Erase(guid);
    ASFW_LOG(Audio,
             "[Lifecycle] AudioCoordinator remote-device-lost owner GUID=0x%016llx "
             "active=%u host=0x%08x (%{public}s)",
             guid, wasActive ? 1U : 0U, hostStatus, ASFW::Logging::IOReturnName(hostStatus));
}

void AudioCoordinator::OnAVCAudioConfigurationReady(uint64_t guid,
                                                   const Model::ASFWAudioDevice& config) noexcept {
    // The host stores the description and publishes or refreshes through its one
    // rule (§4.3). It no longer overwrites the endpoint config under a live nub
    // (AUDIO_DEVICE_HOST.md §5 Δ8).
    host_.OfferDiscoveredDescription(guid, config);
}

void AudioCoordinator::HandleCycleInconsistent() noexcept {
    std::array<uint64_t, Driver::IsochLanes::kMaxLanes> owners{};
    if (lock_) {
        IOLockLock(lock_);
        owners = laneOwner_;
        IOLockUnlock(lock_);
    }

    bool any = false;
    for (const uint64_t guid : owners) {
        if (guid == 0) continue;
        any = true;
        // The host asks the family whether it acts on it (FamilyAdapter::ActsOn):
        // only DICE does; RME, AV/C and MOTU decline it, as their backends did
        // (AUDIO_DEVICE_HOST.md §6 E5).
        if (ServedByHost(guid)) host_.OnRuntimeFault(guid, DuplexRestartReason::kRecoverAfterCycleInconsistent);
    }
    if (!any && ::ASFW::LogConfig::Shared().GetIsochVerbosity() >= 3) {
        ASFW_LOG(Audio, "AudioCoordinator: Ignoring cycleInconsistent with no active audio GUID");
    }
}

bool AudioCoordinator::ServedByHost(uint64_t guid) const noexcept {
    const auto kind = host_.KindForGuid(guid);
    return kind.has_value();
}

IOReturn AudioCoordinator::ClaimTransport(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;
    if (!lock_) return kIOReturnNotReady;
    IOLockLock(lock_);
    if (remoteLostStopResults_.contains(guid)) {
        IOLockUnlock(lock_);
        return kIOReturnNoDevice;
    }
    const uint32_t lane = ClaimLaneLocked(guid);
    const uint32_t count = lanes_.LaneCountIfKnown();
    IOLockUnlock(lock_);
    if (lane == kAllLanes) {
        ASFW_LOG_WARNING(Audio,
                         "AudioCoordinator: transport claim refused GUID=0x%016llx; all %u isoch lanes on this "
                         "controller are streaming",
                         guid, count);
        return kIOReturnBusy;
    }
    ASFW_LOG(Audio, "AudioCoordinator: GUID=0x%016llx claims isoch lane %u of %u", guid, lane, count);
    return kIOReturnSuccess;
}

void AudioCoordinator::ReleaseTransportClaim(uint64_t guid) noexcept {
    if (guid == 0 || !lock_) return;
    // Only a claim nothing came of: a session that attached owns the lane
    // until StopStreaming.
    const bool owned = sessions_.IsStreaming(guid) || sessions_.IsReconciling(guid);
    IOLockLock(lock_);
    if (!owned) ReleaseLaneLocked(guid);
    IOLockUnlock(lock_);
}

IOReturn AudioCoordinator::StartStreaming(uint64_t guid, AudioClockConfig clock) noexcept {
    if (publisher_.IsGeometryChangeBlocked(guid)) return kIOReturnNotReady;
    if (guid == 0) return kIOReturnBadArgument;

    if (lock_) {
        IOLockLock(lock_);
        if (remoteLostStopResults_.contains(guid)) {
            IOLockUnlock(lock_);
            return kIOReturnNoDevice;
        }
        // Usually already claimed by StartIO before the TX resources were
        // allocated; a start without that claim takes a lane here.
        const uint32_t lane = ClaimLaneLocked(guid);
        IOLockUnlock(lock_);
        if (lane == kAllLanes) {
            ASFW_LOG_WARNING(Audio,
                             "AudioCoordinator: StartStreaming busy requested=0x%016llx; every isoch lane is taken",
                             guid);
            return kIOReturnBusy;
        }
    }

    // Ownership can survive a failed stop after host transport has quiesced.
    // Only the session knows whether this is an idempotent running start or
    // whether retained device connections must be cleaned up before restarting.
    const IOReturn kr = sessions_.Attach(guid, clock);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: StartStreaming failed GUID=0x%016llx kr=0x%x (%{public}s)",
                       guid,
                       kr, ASFW::Logging::IOReturnName(kr));
        // Whether this call or an earlier ClaimTransport took the lane:
        // nothing attached, so nothing owns it.
        if (lock_) {
            IOLockLock(lock_);
            ReleaseLaneLocked(guid);
            IOLockUnlock(lock_);
        }
        return kr;
    }

    ASFW_LOG(Audio,
             "AudioCoordinator: StartStreaming ok GUID=0x%016llx",
             guid);
    return kIOReturnSuccess;
}

void AudioCoordinator::NotifyDeviceConfigChanged(uint64_t guid) noexcept {
    if (guid == 0) return;
    if (sessions_.IsStreaming(guid) || sessions_.IsReconciling(guid)) {
        host_.OnDeviceEvent(guid, Host::DeviceEvent::kStreamConfigChanged, 0);
        return;
    }
    host_.RefreshPublication(guid);
}

IOReturn AudioCoordinator::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;

    if (lock_) {
        IOLockLock(lock_);
        if (auto it = remoteLostStopResults_.find(guid); it != remoteLostStopResults_.end()) {
            const auto status = it->second;
            IOLockUnlock(lock_);
            return status;
        }
        const uint32_t lane = LaneOfLocked(guid);
        IOLockUnlock(lock_);
        if (lane == kAllLanes && !sessions_.IsStreaming(guid) && !sessions_.IsReconciling(guid)) {
            // A device that never got a lane has nothing to stop: its start
            // was refused and its unwind must succeed, or the refusal leaves
            // it unable to start for the rest of the driver's life (two
            // 828mkIIs, 2026-10-28). The HAL no longer wants it streaming,
            // though, so detach its session: a failed start leaves the session
            // attached, and a later clock change then ran a full start for a
            // client that was gone (b146: the first start of every rate change
            // after a failed start failed at GraphGeometry). A detach that
            // fails must not fail the unwind.
            const IOReturn detached = sessions_.Detach(guid);
            ASFW_LOG(Audio,
                     "AudioCoordinator: StopStreaming GUID=0x%016llx owns no lane; nothing to stop, session detached kr=0x%x",
                     guid, detached);
            return kIOReturnSuccess;
        }
    }

    const IOReturn kr = sessions_.Detach(guid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: StopStreaming failed GUID=0x%016llx kr=0x%x (%{public}s)",
                       guid,
                       kr, ASFW::Logging::IOReturnName(kr));
        return kr;
    }

    if (lock_) {
        IOLockLock(lock_);
        ReleaseLaneLocked(guid);
        IOLockUnlock(lock_);
    }

    ASFW_LOG(Audio,
             "AudioCoordinator: StopStreaming ok GUID=0x%016llx",
             guid);
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::RequestClockConfig(
    uint64_t guid,
    const AudioClockConfig& desiredClock,
    DuplexRestartReason reason) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }

    const auto record = registry_.SnapshotByGuid(guid);
    if (!record.has_value()) {
        ASFW_LOG_WARNING(Audio,
                         "AudioCoordinator: RequestClockConfig no registry record for GUID=0x%016llx (device not registered yet?)",
                         guid);
        return kIOReturnNotReady;
    }

    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
    if (policy && policy->plan.family == DeviceProfiles::Audio::AudioFamilyProviderId::RmeRegister &&
        desiredClock.sampleRateHz != 48000U) {
        // Fireface control does not expose a software rate switch. Refuse at
        // the coordinator boundary before any protocol read/write is queued.
        return kIOReturnUnsupported;
    }

    const IOReturn kr = sessions_.ChangeClock(guid, desiredClock, reason);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: RequestClockConfig failed GUID=0x%016llx kr=0x%x (%{public}s)",
                       guid,
                       kr, ASFW::Logging::IOReturnName(kr));
        return kr;
    }

    // Keep the endpoint's clock in step with the new device rate so the next
    // StartIO seeds the direct-binding/ZTS clock at the live rate. Without this
    // the binding stays at the publish-time rate (48 kHz) while the device runs
    // 44.1 kHz, and CoreAudio churns StartIO/StopIO on the clock mismatch.
    if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
        Model::ASFWAudioDevice config;
        // An AV/C CONTROL result does not commit an audio configuration. Its
        // host-window transaction installs rate, formations and epoch after
        // STATUS/readback confirmation for every catalog endpoint.
        if (!endpoint->CopyConfig(config) || config.rateFormationCandidates.empty())
            endpoint->SetCurrentSampleRate(desiredClock.sampleRateHz);
    }

    ASFW_LOG(Audio,
             "AudioCoordinator: RequestClockConfig ok GUID=0x%016llx rate=%uHz reason=%u",
             guid,
             desiredClock.sampleRateHz,
             static_cast<unsigned>(reason));
    return kIOReturnSuccess;
}

void AudioCoordinator::BeginTeardown() noexcept {
    ASFW_LOG(Audio, "AudioCoordinator: BeginTeardown");
    teardownRequested_.store(true, std::memory_order_release);
    // Pending restarts go first: once the host drains, nothing can raise one.
    sessions_.BeginTeardown();
    // Block new recovery callbacks before draining the host
    // queue. The coordinator owns this one subscription for every family.
    for (auto& transport : transports_) {
        if (transport) transport->SetTimingLossCallback({});
    }
    host_.BeginTeardown();
    const kern_return_t hostStatus = StopHostTransport("service-teardown");
    if (hostStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: host isoch teardown incomplete kr=0x%08x (%{public}s)",
                       hostStatus, ASFW::Logging::IOReturnName(hostStatus));
    }

    if (lock_) {
        IOLockLock(lock_);
        laneOwner_.fill(0);
        IOLockUnlock(lock_);
    }
}

kern_return_t AudioCoordinator::StopHostTransport(const char* reason,
                                                   bool generationInvalidated,
                                                   uint32_t lane) noexcept {
    kern_return_t status = kIOReturnSuccess;
    for (uint32_t index = 0; index < transports_.size(); ++index) {
        if (lane != kAllLanes && index != lane) continue;
        auto* transport = transports_[index].get();
        if (!transport) continue;
        const kern_return_t kr = generationInvalidated ? transport->StopAllAfterBusReset()
                                                       : transport->StopAll();
        if (kr != kIOReturnSuccess && status == kIOReturnSuccess) status = kr;
    }
    ASFW_LOG(Audio,
             "[Lifecycle] AudioCoordinator host-isoch teardown owner reason=%{public}s lane=%d "
             "generation-invalidated=%u kr=0x%08x (%{public}s)",
             reason, lane == kAllLanes ? -1 : static_cast<int>(lane), generationInvalidated ? 1U : 0U,
             status, ASFW::Logging::IOReturnName(status));
    return status;
}

IOReturn AudioCoordinator::MotuCaptureCommand(uint64_t guid, uint32_t stream,
                                             uint32_t command, std::string& output) noexcept {
    if (captureCommandBusy_.test_and_set(std::memory_order_acquire)) return kIOReturnBusy;
    struct Release final {
        std::atomic_flag& gate;
        ~Release() { gate.clear(std::memory_order_release); }
    } release{captureCommandBusy_};
    if (guid == 0 || command > 2) return kIOReturnBadArgument;
    auto* capture = static_cast<IsochDuplexHostTransport&>(TransportForGuid(guid)).DiagnosticCapture(stream);
    if (!capture) return kIOReturnBadArgument;
    if (teardownRequested_.load(std::memory_order_acquire)) return kIOReturnNotReady;
    if (command == 0) {
        // Only arm a known MOTU endpoint; never label another family's packets as MOTU.
        if (host_.KindForGuid(guid) != AudioBackendKind::MotuRegister) return kIOReturnUnsupported;
        const auto protocol = runtime_.FindShared(guid);
        if (!protocol) return kIOReturnNotReady;
        Wire::MotuRxStreamMetadata metadata{};
        metadata.guid = guid;
        metadata.streamIndex = stream;
        const auto record = registry_.SnapshotByGuid(guid);
        const char* model = record ? DeviceProfiles::Audio::AudioDeviceCatalog::
            MotuModelNameForSwVersion(record->unitSwVersion.value_or(0)) : nullptr;
        snprintf(metadata.model, sizeof(metadata.model), "%s", model ? model : protocol->GetName());
        const auto endpoint = runtime_.FindEndpointRuntime(guid);
        Runtime::DirectAudioBindingSnapshot binding{};
        if (endpoint && endpoint->CopyDirectAudioBinding(binding))
            metadata.sampleRateHz = binding.sampleRateHz;
        return capture->Arm(metadata) ? kIOReturnSuccess : kIOReturnBusy;
    }
    Wire::MotuRxDiagnosticCapture::Snapshot snapshot{};
    if (!capture->CopySnapshot(snapshot)) return kIOReturnBusy;
    if (snapshot.metadata.guid != guid) return kIOReturnNotFound;
    if (!capture->Disarm()) return kIOReturnBusy;
    if (command == 2) {
        output = capture->FormatCaptureSummary();
        if (output.empty()) return kIOReturnBusy;
    }
    return kIOReturnSuccess;
}

void AudioCoordinator::HandleHostTimingLoss(uint64_t guid) noexcept {
    if (lock_) {
        IOLockLock(lock_);
        const bool remoteLost = remoteLostStopResults_.contains(guid);
        IOLockUnlock(lock_);
        if (remoteLost) {
            return;
        }
    }
    if (ServedByHost(guid)) host_.OnRuntimeFault(guid, DuplexRestartReason::kRecoverAfterTimingLoss);
}

std::optional<uint64_t> AudioCoordinator::GetSinglePublishedGuid() const noexcept {
    // AudioNubPublisher is the source of truth for published audio endpoints.
    // This is intentionally used only for debug paths that still lack GUID selection.
    return publisher_.GetSingleGuid();
}

} // namespace ASFW::Audio
