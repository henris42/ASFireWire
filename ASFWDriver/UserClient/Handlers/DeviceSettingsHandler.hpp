// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceSettingsHandler.hpp - User-client access to a device's clock source
// and optical port modes (Audio/Protocols/DeviceSettings.hpp).
//
// Both methods are request/result pairs, as the transactions complete on the
// driver's queue and a user-client call must not wait there.

#pragma once

#include <cstdint>

#include <DriverKit/IOReturn.h>

struct IOUserClientMethodArguments;
class ASFWDriver;

namespace ASFW::UserClient {

class DeviceSettingsHandler {
public:
    explicit DeviceSettingsHandler(::ASFWDriver* driver) : driver_(driver) {}

    /// scalarInput: guid, action (DeviceSettingsAction), setting, value.
    /// scalarOutput: request ID for GetDeviceSettingsResult.
    /// A set is followed by a read, so the result carries the device's state
    /// after the change.
    kern_return_t RequestDeviceSettings(IOUserClientMethodArguments* args);

    /// scalarInput: request ID. structureOutput: DeviceSettingsResultWire.
    /// kIOReturnNotReady while the transaction is in flight, kIOReturnNotFound
    /// for an unknown or already collected request.
    kern_return_t GetDeviceSettingsResult(IOUserClientMethodArguments* args);

private:
    ::ASFWDriver* driver_;
};

} // namespace ASFW::UserClient
