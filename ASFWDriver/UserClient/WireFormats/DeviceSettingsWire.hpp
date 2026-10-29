// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceSettingsWire.hpp - Device settings result for the user client.
//
// Reading or changing a device setting is a bus transaction, so the user
// client works like the raw FCP path: RequestDeviceSettings starts it and
// returns a request ID, GetDeviceSettingsResult returns this structure once the
// transaction completed (kIOReturnNotReady while it is in flight). Little
// endian, the app's native order.

#pragma once

#include "../../Audio/Protocols/DeviceSettings.hpp"

#include <cstdint>

namespace ASFW::UserClient::Wire {

inline constexpr uint32_t kDeviceSettingsWireVersion = 1;

enum DeviceSettingsAction : uint32_t {
    kDeviceSettingsActionRead = 0,
    kDeviceSettingsActionSet = 1,
};

struct DeviceSettingsResultWire {
    uint32_t version;            ///< kDeviceSettingsWireVersion
    uint32_t status;             ///< kern_return_t of the transaction
    uint64_t guid;
    uint32_t supportedSettings;
    uint32_t clockSource;
    uint32_t clockSourceOptions;
    uint32_t opticalInputMode;
    uint32_t opticalOutputMode;
    uint32_t opticalModeOptions;
    uint32_t clockRaw;
    uint32_t opticalRaw;
    // Added after the first release, without a version change: a reader tells
    // by the size (60 bytes) whether they are there, and a reader that predates
    // them takes the first 48 bytes as before.
    uint32_t analogInputSwitches;
    uint32_t inputLevelRaw;
    uint32_t inputBoostRaw;
} __attribute__((packed));

static_assert(sizeof(DeviceSettingsResultWire) == 60,
              "the app reads the extended fields by size, and its result buffer is 64 bytes");

} // namespace ASFW::UserClient::Wire
