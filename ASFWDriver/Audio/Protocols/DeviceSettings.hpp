// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceSettings.hpp - Host-settable device configuration, family-neutral.
//
// A device protocol that lets the host change its clock source, the
// protocol carried on its optical ports or its analog input switches implements
// IDeviceProtocol's ReadDeviceSettings/ApplyDeviceSetting with these types. The user client
// exposes them unchanged (UserClient/WireFormats/DeviceSettingsWire.hpp), and
// the app persists chosen values per device GUID and re-applies them at boot.

#pragma once

#include <cstdint>

namespace ASFW::Audio {

enum class DeviceSetting : uint32_t {
    ClockSource = 1,
    OpticalInputMode = 2,
    OpticalOutputMode = 3,
    /// Analog input reference levels, one bit per input: bit n set puts analog
    /// input n+1 at +4 dBu, clear at -10 dBV.
    InputLevels = 4,
    /// Analog input boost, one bit per input: bit n set adds 6 dB to analog
    /// input n+1.
    InputBoosts = 5,
};

/// Clock source codes. The MOTU v2 register codes are used as they are
/// (MotuRegisters.hpp ClockSourceV2); other families map onto them.
enum class DeviceClockSource : uint32_t {
    Internal = 0,
    AdatOptical = 1,
    Spdif = 2,
    BusSph = 3,          ///< Source packet headers (host/bus clock).
    WordClockBnc = 4,
    AdatDsub = 5,
    AesEbuXlr = 7,
};

/// What an optical port carries.
enum class DeviceOpticalMode : uint32_t {
    Off = 0,
    Adat = 1,
    Spdif = 2,          ///< TOSLINK S/PDIF.
};

struct DeviceSettingsSnapshot {
    /// Bit (1 << DeviceSetting) for every setting this device can change.
    uint32_t supportedSettings{0};
    uint32_t clockSource{0};
    /// Bit (1 << code) for every DeviceClockSource the device offers.
    uint32_t clockSourceOptions{0};
    uint32_t opticalInputMode{0};
    uint32_t opticalOutputMode{0};
    /// Bit (1 << mode) for every DeviceOpticalMode the optical ports accept.
    uint32_t opticalModeOptions{0};
    /// The registers behind the decoded values, for diagnostics.
    uint32_t clockRaw{0};
    uint32_t opticalRaw{0};
    /// Bit n: analog input n+1 has the level and boost switches. InputLevels and
    /// InputBoosts values and the two registers below use the same bit order;
    /// only these bits are the inputs' state.
    uint32_t analogInputSwitches{0};
    uint32_t inputLevelRaw{0};
    uint32_t inputBoostRaw{0};
};

[[nodiscard]] constexpr uint32_t SettingBit(DeviceSetting setting) noexcept {
    return 1u << static_cast<uint32_t>(setting);
}

} // namespace ASFW::Audio
