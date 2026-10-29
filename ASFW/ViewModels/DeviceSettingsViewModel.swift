//
//  DeviceSettingsViewModel.swift
//  ASFW
//
//  One controller's devices with host-settable configuration (today: MOTU
//  clock source, optical modes and analog input switches), their current
//  state, and the changes the user asks for.
//

import Combine
import Foundation
import IOKit

@MainActor
final class DeviceSettingsViewModel: ObservableObject {
    struct Device: Identifiable, Equatable {
        let guid: UInt64
        let name: String
        var id: UInt64 { guid }
    }

    @Published private(set) var devices: [Device] = []
    @Published var selectedGUID: UInt64? {
        didSet { if oldValue != selectedGUID { Task { await reload() } } }
    }
    @Published private(set) var snapshot: DeviceSettingsSnapshot?
    @Published private(set) var isBusy = false
    @Published private(set) var message: String?

    private let connector: ASFWDriverConnector
    private let debugVM: DebugViewModel
    private let store = DeviceConfigStore()
    private var cancellables = Set<AnyCancellable>()
    /// What the login agent re-applies for the selected device.
    @Published private(set) var remembered: [DeviceSetting: UInt32] = [:]
    /// Stream-start experiments, as last sent to the driver (driver-wide, reset by a driver restart).
    @Published var experiment = MotuExperiment()
    @Published private(set) var experimentMessage: String?

    init(connector: ASFWDriverConnector, debugVM: DebugViewModel) {
        self.connector = connector
        self.debugVM = debugVM
        debugVM.$discoveredDevices
            .map { devices in
                devices.filter { $0.vendorId == ASFWDriverConnector.motuVendorID }
                    .map { Device(guid: $0.guid, name: $0.displayLabel) }
            }
            .removeDuplicates()
            .receive(on: DispatchQueue.main)
            .sink { [weak self] devices in self?.updateDevices(devices) }
            .store(in: &cancellables)
    }

    private func updateDevices(_ newDevices: [Device]) {
        devices = newDevices
        if let selectedGUID, newDevices.contains(where: { $0.guid == selectedGUID }) { return }
        selectedGUID = newDevices.first?.guid
    }

    func applyExperiment() {
        let kr = connector.setMotuExperiment(experiment)
        experimentMessage = kr == KERN_SUCCESS
            ? (experiment.isDefault ? "Experiments off. Stop and start playback to apply." : "Sent. Stop and start playback to apply.")
            : "Not sent: \(connector.interpretIOReturn(kr))"
    }

    func forgetRemembered() {
        guard let guid = selectedGUID else { return }
        store.forget(guid: guid)
        remembered = [:]
    }

    func reload() async {
        guard let guid = selectedGUID else {
            snapshot = nil
            return
        }
        remembered = store.remembered(for: guid)
        isBusy = true
        defer { isBusy = false }
        switch await connector.performDeviceSettings(guid: guid) {
        case .ready(let fresh):
            snapshot = fresh
            message = nil
        case .failed(let kr):
            snapshot = nil
            message = "Could not read the device settings: \(connector.interpretIOReturn(kr))"
        case .pending:
            break
        }
    }

    func apply(_ setting: DeviceSetting, value: UInt32) async {
        guard let guid = selectedGUID else { return }
        isBusy = true
        defer { isBusy = false }
        switch await connector.performDeviceSettings(guid: guid, set: setting, value: value) {
        case .ready(let fresh):
            snapshot = fresh
            switch setting {
            case .opticalInputMode, .opticalOutputMode:
                message = "Optical mode changed; the device's streams are republished with the new channel layout."
            case .clockSource, .inputLevels, .inputBoosts:
                message = nil
            }
            store.remember(guid: guid, setting: setting, value: value)
            remembered = store.remembered(for: guid)
        case .failed(let kr):
            message = "\(setting.title) was not changed: \(connector.interpretIOReturn(kr))"
            await reload()
        case .pending:
            break
        }
    }
}
