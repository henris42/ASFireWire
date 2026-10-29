//
//  DriverConnector+DeviceSettings.swift
//  ASFW
//
//  Clock source, optical port modes and analog input switches of a device,
//  through the user client's request/result pair (selectors 69/70). Reading or changing them is a bus
//  transaction, so the driver answers asynchronously and the app polls.
//

import Foundation
import IOKit

extension ASFWDriverConnector {
    static let motuVendorID: UInt32 = 0x0001F2

    /// Start a read (setting/value ignored) or a change. Returns the request ID.
    func requestDeviceSettings(guid: UInt64, set setting: DeviceSetting? = nil,
                               value: UInt32 = 0) -> UInt64? {
        guard isConnected, connection != 0, guid != 0 else { return nil }
        var input: [UInt64] = [guid, setting == nil ? 0 : 1, UInt64(setting?.rawValue ?? 0), UInt64(value)]
        var output: [UInt64] = [0]
        var outputCount: UInt32 = 1
        let kr = IOConnectCallScalarMethod(connection,
                                           Method.requestDeviceSettings.rawValue,
                                           &input, UInt32(input.count),
                                           &output, &outputCount)
        guard kr == KERN_SUCCESS, outputCount == 1 else {
            log("requestDeviceSettings failed: \(interpretIOReturn(kr))", level: .error)
            return nil
        }
        return output[0]
    }

    func deviceSettingsResult(requestID: UInt64) -> DeviceSettingsOutcome {
        guard isConnected, connection != 0 else { return .failed(kIOReturnNotReady) }
        var input: UInt64 = requestID
        var size = 64
        var buffer = Data(count: size)
        let kr = buffer.withUnsafeMutableBytes { raw -> kern_return_t in
            IOConnectCallMethod(connection,
                                Method.getDeviceSettingsResult.rawValue,
                                &input, 1,
                                nil, 0,
                                nil, nil,
                                raw.baseAddress, &size)
        }
        if kr == kIOReturnNotReady { return .pending }
        guard kr == KERN_SUCCESS else { return .failed(kr) }
        let data = buffer.prefix(size)
        if let status = DeviceSettingsSnapshot.wireStatus(data), status != KERN_SUCCESS {
            return .failed(status)
        }
        guard let snapshot = DeviceSettingsSnapshot(wire: data) else { return .failed(kIOReturnBadMessageID) }
        return .ready(snapshot)
    }

    /// MOTU stream-start experiments (driver: MotuExperiments.hpp). Process-wide in
    /// the driver, not persisted, applied at the next playback start.
    func setMotuExperiment(_ experiment: MotuExperiment) -> kern_return_t {
        guard isConnected, connection != 0 else { return kIOReturnNotReady }
        var input: [UInt64] = [UInt64(experiment.flags), UInt64(experiment.extraNoDataPackets),
                               UInt64(experiment.presentationLeadTicks)]
        let kr = IOConnectCallScalarMethod(connection, Method.setMotuExperiment.rawValue,
                                           &input, UInt32(input.count), nil, nil)
        if kr != KERN_SUCCESS {
            log("setMotuExperiment failed: \(interpretIOReturn(kr))", level: .error)
        }
        return kr
    }

    /// Request, then poll until the transaction completes or `timeout` passes.
    func performDeviceSettings(guid: UInt64, set setting: DeviceSetting? = nil, value: UInt32 = 0,
                               timeout: TimeInterval = 4) async -> DeviceSettingsOutcome {
        guard let requestID = requestDeviceSettings(guid: guid, set: setting, value: value) else {
            return .failed(kIOReturnNotReady)
        }
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            let outcome = deviceSettingsResult(requestID: requestID)
            if case .pending = outcome {
                try? await Task.sleep(nanoseconds: 50_000_000)
                continue
            }
            return outcome
        }
        return .failed(kIOReturnTimeout)
    }
}
