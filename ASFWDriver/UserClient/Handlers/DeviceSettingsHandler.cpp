// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "DeviceSettingsHandler.hpp"

#include <net.asfw.driver/ASFWDriver.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>

#include <memory>
#include <unordered_map>

#include "../../Audio/Core/AudioCoordinator.hpp"
#include "../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../Audio/Protocols/IDeviceProtocol.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Service/DriverContext.hpp"
#include "../WireFormats/DeviceSettingsWire.hpp"

namespace ASFW::UserClient {

namespace {

struct PendingResult {
    bool ready{false};
    kern_return_t status{kIOReturnNotReady};
    uint64_t guid{0};
    Audio::DeviceSettingsSnapshot snapshot{};
};

struct ResultStore {
    IOLock* lock{IOLockAlloc()};
    uint64_t nextRequestID{1};
    std::unordered_map<uint64_t, PendingResult> results;
};

// Results nobody collected must not accumulate across a long session.
constexpr size_t kMaxStoredResults = 32;

ResultStore& Store() {
    static ResultStore store{};
    return store;
}

void Complete(uint64_t requestID, kern_return_t status, const Audio::DeviceSettingsSnapshot& snapshot) {
    auto& store = Store();
    if (!store.lock) return;
    IOLockLock(store.lock);
    if (auto it = store.results.find(requestID); it != store.results.end()) {
        it->second.ready = true;
        it->second.status = status;
        it->second.snapshot = snapshot;
    }
    IOLockUnlock(store.lock);
}

} // namespace

kern_return_t DeviceSettingsHandler::RequestDeviceSettings(IOUserClientMethodArguments* args) {
    if (!args || !args->scalarInput || args->scalarInputCount < 4 || !args->scalarOutput ||
        args->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }
    const uint64_t guid = args->scalarInput[0];
    const auto action = static_cast<uint32_t>(args->scalarInput[1]);
    const auto setting = static_cast<uint32_t>(args->scalarInput[2]);
    const auto value = static_cast<uint32_t>(args->scalarInput[3]);
    if (guid == 0 || action > Wire::kDeviceSettingsActionSet) {
        return kIOReturnBadArgument;
    }

    auto* context = driver_ ? static_cast<ServiceContext*>(driver_->GetServiceContext()) : nullptr;
    if (!context || !context->deps.audioRuntimeRegistry) {
        return kIOReturnNotReady;
    }
    std::shared_ptr<Audio::IDeviceProtocol> protocol = context->deps.audioRuntimeRegistry->FindShared(guid);
    if (!protocol) {
        return kIOReturnNotFound;
    }
    auto coordinator = context->audioCoordinator;

    auto& store = Store();
    if (!store.lock) {
        return kIOReturnNoMemory;
    }
    IOLockLock(store.lock);
    if (store.results.size() >= kMaxStoredResults) {
        // Drop the oldest collected-or-not entry; IDs grow monotonically.
        auto oldest = store.results.begin();
        for (auto it = store.results.begin(); it != store.results.end(); ++it) {
            if (it->first < oldest->first) oldest = it;
        }
        store.results.erase(oldest);
    }
    const uint64_t requestID = store.nextRequestID++;
    store.results[requestID] = PendingResult{.guid = guid};
    IOLockUnlock(store.lock);
    args->scalarOutput[0] = requestID;
    args->scalarOutputCount = 1;

    // The protocol is kept alive by the captured shared_ptr until the last callback.
    auto readBack = [protocol, requestID](kern_return_t applyStatus) {
        if (applyStatus != kIOReturnSuccess) {
            Complete(requestID, applyStatus, {});
            return;
        }
        protocol->ReadDeviceSettings([requestID](IOReturn status, Audio::DeviceSettingsSnapshot snapshot) {
            Complete(requestID, status, snapshot);
        });
    };

    if (action == Wire::kDeviceSettingsActionRead) {
        readBack(kIOReturnSuccess);
        return kIOReturnSuccess;
    }

    const auto which = static_cast<Audio::DeviceSetting>(setting);
    ASFW_LOG(UserClient, "DeviceSettings: GUID=0x%016llx set %u = %u", guid, setting, value);
    protocol->ApplyDeviceSetting(which, value,
        [guid, which, coordinator, readBack](IOReturn status) {
            if (status == kIOReturnSuccess && coordinator &&
                (which == Audio::DeviceSetting::OpticalInputMode ||
                 which == Audio::DeviceSetting::OpticalOutputMode)) {
                // The stream layout follows the optical modes: republish the
                // endpoint, restarting a running stream.
                coordinator->NotifyDeviceConfigChanged(guid);
            }
            readBack(status);
        });
    return kIOReturnSuccess;
}

kern_return_t DeviceSettingsHandler::GetDeviceSettingsResult(IOUserClientMethodArguments* args) {
    if (!args || !args->scalarInput || args->scalarInputCount < 1) {
        return kIOReturnBadArgument;
    }
    auto& store = Store();
    if (!store.lock) {
        return kIOReturnNoMemory;
    }
    const uint64_t requestID = args->scalarInput[0];
    PendingResult result{};
    bool found = false;
    IOLockLock(store.lock);
    if (auto it = store.results.find(requestID); it != store.results.end()) {
        found = true;
        if (it->second.ready) {
            result = it->second;
            store.results.erase(it);
        }
    }
    IOLockUnlock(store.lock);
    if (!found) return kIOReturnNotFound;
    if (!result.ready) return kIOReturnNotReady;

    Wire::DeviceSettingsResultWire wire{};
    wire.version = Wire::kDeviceSettingsWireVersion;
    wire.status = static_cast<uint32_t>(result.status);
    wire.guid = result.guid;
    wire.supportedSettings = result.snapshot.supportedSettings;
    wire.clockSource = result.snapshot.clockSource;
    wire.clockSourceOptions = result.snapshot.clockSourceOptions;
    wire.opticalInputMode = result.snapshot.opticalInputMode;
    wire.opticalOutputMode = result.snapshot.opticalOutputMode;
    wire.opticalModeOptions = result.snapshot.opticalModeOptions;
    wire.clockRaw = result.snapshot.clockRaw;
    wire.opticalRaw = result.snapshot.opticalRaw;
    wire.analogInputSwitches = result.snapshot.analogInputSwitches;
    wire.inputLevelRaw = result.snapshot.inputLevelRaw;
    wire.inputBoostRaw = result.snapshot.inputBoostRaw;
    args->structureOutput = OSData::withBytes(&wire, sizeof(wire));
    return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

} // namespace ASFW::UserClient
