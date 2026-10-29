// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioCoordinator.hpp
// Central audio control-plane entry point. Owns audio nubs and routes
// start/stop to the explicit DICE, MOTU and AV/C backends.

#pragma once

#include "IAVCAudioConfigListener.hpp"
#include "AudioNubPublisher.hpp"
#include "../Protocols/Backends/IsochDuplexHostTransport.hpp"
#include "../../Isoch/IsochLanes.hpp"
#include "../Session/AudioSessions.hpp"
#include "../Host/AudioDeviceHost.hpp"
#include "../Host/AvcFamilyAdapter.hpp"
#include "../Host/DiceFamilyAdapter.hpp"
#include "../Host/MotuFamilyAdapter.hpp"
#include "../Host/RmeFamilyAdapter.hpp"

#include "../../Logging/Logging.hpp"

#include "../../Discovery/IDeviceManager.hpp"

#include <DriverKit/IOLib.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

class IOService;

namespace ASFW::CMP {
class CMPClient;
}

namespace ASFW::Audio {

class AudioRuntimeRegistry;

class AudioCoordinator final : public Discovery::IDeviceObserver,
                               public IAVCAudioConfigListener {
public:
    AudioCoordinator(IOService* driver,
                     Discovery::IDeviceManager& deviceManager,
                     Discovery::DeviceRegistry& registry,
                     AudioRuntimeRegistry& runtime,
                     Driver::IsochLanes& lanes,
                     Driver::HardwareInterface& hardware,
                     DICE::DiceNotificationRouter& diceNotifications) noexcept;
    ~AudioCoordinator() noexcept override;

    AudioCoordinator(const AudioCoordinator&) = delete;
    AudioCoordinator& operator=(const AudioCoordinator&) = delete;

    void SetCMPClient(ASFW::CMP::CMPClient* client) noexcept;
    // The bus's IRM client; the audio session reserves stream resources with it.
    void SetIRMClient(ASFW::IRM::IRMClient* client) noexcept { sessions_.SetIrmClient(client); }

    // IDeviceObserver
    void OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceRemoved(Discovery::Guid64 guid) override;

    // IAVCAudioConfigListener
    void OnAVCAudioConfigurationReady(uint64_t guid,
                                      const Model::ASFWAudioDevice& config) noexcept override;
    [[nodiscard]] bool IsAudioActive(uint64_t guid) const noexcept override {
        return sessions_.IsStreaming(guid) || sessions_.IsReconciling(guid);
    }
    void HandleCycleInconsistent() noexcept;
    // Some session owns an isoch lane: starting, running, or waiting out a
    // bus reset. Read by the controller's gap policy.
    [[nodiscard]] bool HasActiveSession() const noexcept;
    // The GUID owning the lowest claimed lane, 0 when none is claimed.
    [[nodiscard]] uint64_t ActiveGuid() const noexcept;
    // The GUID whose device streams on `lane`, 0 when the lane is free.
    [[nodiscard]] uint64_t GuidForLane(uint32_t lane) const noexcept;
    // The isoch service of the lane `guid` has claimed; null before a claim.
    // The nub allocates its transmit resources on it.
    [[nodiscard]] Driver::IsochService* IsochServiceForGuid(uint64_t guid) noexcept;
    // A stable per-GUID transport that forwards each call to the lane the
    // GUID holds at that moment; what sessions and the host are given.
    [[nodiscard]] IIsochDuplexHostTransport& SessionTransportForGuid(uint64_t guid) noexcept;
    // After async generation work has been aborted, before ROM discovery.
    void HandleBusReset() noexcept;

    // Each streaming device claims an isoch lane (a pair of IT/IR contexts
    // with its own transport) before it allocates anything; a device is
    // refused only when every lane the controller offers is taken, and the
    // running ones are left alone (two 828mkIIs, 2026-10-28: the second
    // StartIO re-allocated the shared TX slot under the first and both died).
    // A start takes the lowest free lane.
    [[nodiscard]] IOReturn ClaimTransport(uint64_t guid) noexcept;
    void ReleaseTransportClaim(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn StartStreaming(uint64_t guid, AudioClockConfig clock = {}) noexcept;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn RequestClockConfig(
        uint64_t guid,
        const AudioClockConfig& desiredClock,
        DuplexRestartReason reason) noexcept;
    void BeginTeardown() noexcept;
    // The timer that keeps device-event quiet periods; installed by
    // composition before device callbacks begin.
    void SetSessionTimer(Scheduling::ITimerScheduler* timer) noexcept {
        sessions_.SetTimerScheduler(timer);
        host_.SetTimerScheduler(timer);
    }
    void HandleHostTimingLoss(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn MotuCaptureCommand(uint64_t guid, uint32_t stream,
                                            uint32_t command, std::string& output) noexcept;
    /// The host changed a setting that alters the device's stream layout
    /// (DeviceSettings.hpp optical modes): restart a running stream, otherwise
    /// re-describe and republish the endpoint.
    void NotifyDeviceConfigChanged(uint64_t guid) noexcept;

    [[nodiscard]] ASFWAudioNub* GetNub(uint64_t guid) const noexcept { return publisher_.GetNub(guid); }

#ifdef ASFW_HOST_TEST
    [[nodiscard]] Host::AudioDeviceHost& HostForTesting() noexcept { return host_; }
    [[nodiscard]] std::optional<Session::SessionSnapshot> SessionSnapshotForTesting(uint64_t guid) const noexcept {
        return sessions_.Snapshot(guid);
    }
#endif

    /// Debug helper: return the GUID if exactly one audio nub is published.
    [[nodiscard]] std::optional<uint64_t> GetSinglePublishedGuid() const noexcept;

private:
    // True when `guid`'s current policy names a family the host serves.
    [[nodiscard]] bool ServedByHost(uint64_t guid) const noexcept;
    // Stop one lane's transport, or every lane's when `lane` is kAllLanes.
    static constexpr uint32_t kAllLanes = ~0U;
    [[nodiscard]] kern_return_t StopHostTransport(const char* reason,
                                                   bool generationInvalidated = false,
                                                   uint32_t lane = kAllLanes) noexcept;
    // The transport a GUID's session streams through: its claimed lane, else
    // its last lane, else lane 0. Resolved on every call: a session is built
    // when the device appears, long before any lane is claimed (b122,
    // 2026-10-28: every session ran on lane 0 because the lookup happened once
    // at session creation).
    [[nodiscard]] IIsochDuplexHostTransport& TransportForGuid(uint64_t guid) noexcept;
    class LaneTransport;
    [[nodiscard]] IsochDuplexHostTransport* TransportForLane(uint32_t lane) noexcept;
    // Lock held. Give `guid` a lane: its current one, its sticky one if free,
    // else the first free lane. Returns the lane or kAllLanes when none.
    [[nodiscard]] uint32_t ClaimLaneLocked(uint64_t guid) noexcept;
    [[nodiscard]] uint32_t LaneOfLocked(uint64_t guid) const noexcept;
    void ReleaseLaneLocked(uint64_t guid) noexcept;
    // Wire a lane's service callbacks (wakes, timing loss) to the coordinator.
    void InstallLaneCallbacks(uint32_t lane) noexcept;
    void WakeLaneOwner(uint32_t lane, uint64_t generation, bool ztsAnchor) noexcept;


    AudioNubPublisher publisher_;
    Discovery::IDeviceManager& deviceManager_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Driver::IsochLanes& lanes_;
    Driver::HardwareInterface& hardware_;
    // One transport per lane. Backends borrow the neutral interface of their
    // device's lane; none owns a second wrapper around an IsochService.
    std::array<std::unique_ptr<IsochDuplexHostTransport>, Driver::IsochLanes::kMaxLanes> transports_{};
    // laneOwner_[lane] is the GUID claiming it (0 = free); laneByGuid_ is the
    // lane a GUID last used (its transport after a stop, for diagnostics).
    std::array<uint64_t, Driver::IsochLanes::kMaxLanes> laneOwner_{};
    std::unordered_map<uint64_t, uint32_t> laneByGuid_{};
    std::unordered_map<uint64_t, std::unique_ptr<LaneTransport>> sessionTransports_{};
    std::atomic_flag captureCommandBusy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> teardownRequested_{false};
    Session::AudioSessions sessions_;
    // Every family runs on the host (documentation/AUDIO_DEVICE_HOST.md §6).
    // Adapters are declared before the host so they outlive its teardown.
    Host::DiceFamilyAdapter diceAdapter_;
    Host::RmeFamilyAdapter rmeAdapter_;
    Host::AvcFamilyAdapter avcAdapter_;
    Host::MotuFamilyAdapter motuAdapter_;
    Host::AudioDeviceHost host_;

    IOLock* lock_{nullptr};
    // A CoreAudio StopIO can arrive after discovery has retired the GUID. Keep
    // that callback from re-entering a backend that now has no remote device.
    std::unordered_map<uint64_t, IOReturn> remoteLostStopResults_{};
};

} // namespace ASFW::Audio
