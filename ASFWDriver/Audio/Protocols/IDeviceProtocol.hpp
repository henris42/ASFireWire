// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// IDeviceProtocol.hpp - Interface for device-specific protocol handlers

#pragma once
#include "../Runtime/ResolvedAudioConfiguration.hpp"

#include "AudioTypes.hpp"
#include "DeviceSettings.hpp"
#include "../../Discovery/DeviceRouteToken.hpp"

#include <DriverKit/IOReturn.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ASFW::AVC {
class IAvcUnit;
}

namespace ASFW::IRM {
    class IRMClient;
}

namespace ASFW::Audio {
class FamilyDriver;
}

namespace ASFW::Audio {

/// Interface for device-specific protocol handlers
///
/// Device protocols are instantiated by CreateFamilyDeviceProtocol when a
/// audio endpoint is resolved during discovery. Each protocol handler
/// encapsulates vendor-specific control logic (DSP, routing, etc.).
class IDeviceProtocol {
public:
    virtual ~IDeviceProtocol() = default;
    
    /// Initialize the protocol (read device state, cache parameters)
    /// @return kIOReturnSuccess on success
    virtual IOReturn Initialize() = 0;
    
    /// Shutdown the protocol (release resources)
    /// @return kIOReturnSuccess on success
    virtual IOReturn Shutdown() = 0;
    
    /// Get human-readable device name
    virtual const char* GetName() const = 0;
    
    /// Check if device supports DSP effects
    virtual bool HasDsp() const { return false; }
    
    /// Check if device supports hardware mixer
    virtual bool HasMixer() const { return false; }

    /// Query runtime-discovered audio stream capabilities.
    /// Returns true when the protocol has authoritative stream caps (e.g. DICE TX/RX stream formats).
    virtual bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
        (void)outCaps;
        return false;
    }

    // Immutable protocol-discovered formation snapshot, distinct from the
    // current stream observation and from hardware qualification evidence.
    virtual std::shared_ptr<const std::vector<Runtime::RateFormation>> RateFormations() const {
        return {};
    }
    virtual void ReadRateObservation(std::function<void(IOReturn, RateHardwareObservation)> callback) {
        callback(kIOReturnUnsupported, {});
    }

    /// Read the device's stream geometry into the cache GetRuntimeAudioStreamCaps
    /// serves, then call back. Asynchronous, so it is safe on the Default queue
    /// (nub publication). A protocol with nothing to read succeeds at once.
    virtual void EnsureRuntimeStreamGeometry(std::function<void(IOReturn)> callback) {
        callback(kIOReturnSuccess);
    }

    /// Query per-channel device labels discovered from the protocol's stream
    /// format (e.g. DICE TX/RX name sections). `inNames` is host input/capture,
    /// `outNames` is host output/playback, both in channel order; an empty entry
    /// means "no label for that channel". Returns true only when authoritative
    /// labels are available (caps loaded); callers fall back to synthesized
    /// names otherwise.
    virtual bool GetChannelLabels(std::vector<std::string>& inNames,
                                  std::vector<std::string>& outNames) const {
        (void)inNames;
        (void)outNames;
        return false;
    }

    /// The device's clock sources a user can choose, and the one it uses as of
    /// the last clock read (no bus traffic). `choices` pairs the value
    /// ApplyDeviceSetting(DeviceSetting::ClockSource, value) takes with its
    /// name. False: the device has no clock source choice to publish.
    virtual bool GetClockSourceChoices(std::vector<std::pair<uint32_t, std::string>>& choices,
                                       uint32_t& current) const {
        (void)choices;
        (void)current;
        return false;
    }

    using VoidCallback = std::function<void(IOReturn)>;


    /// Optional teardown hook to stop device-side duplex state.
    virtual IOReturn StopDuplex() {
        return kIOReturnUnsupported;
    }


    /// The protocol's streaming driver, for the audio session. Null: the device
    /// does not stream.
    virtual FamilyDriver* AsFamilyDriver() noexcept { return nullptr; }


    /// Update volatile runtime context that can change across bus resets.
    /// `avcUnit` is the device's AV/C unit (owned: the audio nub calls in on its
    /// own queue), or null for a device without AV/C. Every frame goes through
    /// IAvcUnit::Submit, which checks the route and the command allowlist.
    virtual void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                      std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) {
        (void)route;
        (void)avcUnit;
    }

    // Explicit discovery/reset hook, including idle devices. The caller retains
    // the protocol until completion. Register protocols restore notification IO
    // here; ordinary protocols have no reset registration work.
    virtual void RebindNotifications(VoidCallback callback) { callback(kIOReturnSuccess); }

    /// Check if protocol can expose/control a boolean control.
    // These virtuals intentionally match the host-facing `(class, element[, value])` contract.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    virtual bool SupportsBooleanControl(uint32_t classIdFourCC,
                                        uint32_t element) const {
        (void)classIdFourCC;
        (void)element;
        return false;
    }

    /// Read protocol-backed boolean control value.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    virtual IOReturn GetBooleanControlValue(uint32_t classIdFourCC,
                                            uint32_t element,
                                            bool& outValue) {
        (void)classIdFourCC;
        (void)element;
        (void)outValue;
        return kIOReturnUnsupported;
    }

    /// Write protocol-backed boolean control value.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    virtual IOReturn SetBooleanControlValue(uint32_t classIdFourCC,
                                            uint32_t element,
                                            bool value) {
        (void)classIdFourCC;
        (void)element;
        (void)value;
        return kIOReturnUnsupported;
    }

    /// Host-settable configuration (DeviceSettings.hpp): the device's current
    /// clock source, optical port modes and analog input switches with the
    /// options it offers. The default has none.
    virtual void ReadDeviceSettings(std::function<void(IOReturn, DeviceSettingsSnapshot)> callback) {
        if (callback) callback(kIOReturnUnsupported, DeviceSettingsSnapshot{});
    }

    /// Change one setting on the device. Completes once the register write has
    /// been acknowledged; the caller re-reads the settings and, for an optical
    /// mode, republishes the endpoint since the stream layout changed.
    virtual void ApplyDeviceSetting(DeviceSetting setting, uint32_t value,
                                    std::function<void(IOReturn)> callback) {
        (void)setting;
        (void)value;
        if (callback) callback(kIOReturnUnsupported);
    }

    /// The settings as the device last reported them (ReadDeviceSettings),
    /// with any change applied since; empty before the first read. The host
    /// keeps them when the device leaves, to put them back when it returns.
    [[nodiscard]] virtual std::optional<DeviceSettingsSnapshot> LastDeviceSettings() const {
        return std::nullopt;
    }

    /// Set the device's sample rate while nothing streams (the host puts a
    /// returning device back to the rate it had). Completes once the write
    /// was acknowledged; the caller re-reads the geometry.
    virtual void ApplySampleRate(uint32_t rateHz, std::function<void(IOReturn)> callback) {
        (void)rateHz;
        if (callback) callback(kIOReturnUnsupported);
    }
};

} // namespace ASFW::Audio
