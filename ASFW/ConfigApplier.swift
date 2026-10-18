//
//  ConfigApplier.swift
//  ASFW
//
//  Headless mode behind `ASFW --apply-config`, run by the login agent
//  install.sh installs (/Library/LaunchAgents/net.asfw.apply-config.plist).
//  It watches every FireWire controller the driver claims, and whenever a
//  device with remembered settings (DeviceConfigStore) appears or comes back
//  with a new bus generation, reads the device's current settings and writes
//  the ones that differ. The app itself applies a change immediately; this
//  makes it hold across reboots, re-plugs and power cycles of the device.
//

import Foundation
import IOKit

@MainActor
final class ConfigApplier {
    private let store = DeviceConfigStore()
    /// One connector per controller, keyed by the ASFWDriver registry entry ID.
    private var connectors: [UInt64: ASFWDriverConnector] = [:]
    /// Bus generation at which each device last had its settings checked.
    private var checkedGeneration: [UInt64: UInt32] = [:]
    private var applying = Set<UInt64>()

    static let pollInterval: TimeInterval = 5

    /// Runs until the process is killed; returns only if nothing can ever work.
    static func runForever() -> Int32 {
        let applier = ConfigApplier()
        say("Device settings agent started (build \(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?")).")
        let remembered = applier.store.rememberedGUIDs()
        say(remembered.isEmpty
            ? "No remembered device settings yet; waiting for the app to save some."
            : "Remembered settings for \(remembered.count) device(s): \(remembered.map { String(format: "%016llX", $0) }.joined(separator: ", "))")
        applier.poll()
        while true {
            _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.5))
        }
    }

    private func poll() {
        reconcileControllers()
        for connector in connectors.values where connector.isConnected {
            checkDevices(on: connector)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.pollInterval) { [weak self] in
            self?.poll()
        }
    }

    private func reconcileControllers() {
        let cards = CardRegistry.enumerate()
        let ids = Set(cards.map(\.registryEntryID))
        for id in connectors.keys where !ids.contains(id) {
            connectors[id]?.disconnect()
            connectors.removeValue(forKey: id)
            Self.say("Controller \(id) gone.")
        }
        for card in cards where connectors[card.registryEntryID] == nil {
            let connector = ASFWDriverConnector(registryEntryID: card.registryEntryID)
            connector.connect(forceAttempt: true)
            connectors[card.registryEntryID] = connector
            Self.say("Watching \(card.displayName).")
        }
    }

    private func checkDevices(on connector: ASFWDriverConnector) {
        guard let devices = connector.getDiscoveredDevices() else { return }
        for device in devices where device.vendorId == ASFWDriverConnector.motuVendorID {
            let wanted = store.remembered(for: device.guid)
            guard !wanted.isEmpty else { continue }
            guard checkedGeneration[device.guid] != device.generation, !applying.contains(device.guid) else { continue }
            applying.insert(device.guid)
            Task { [weak self] in
                await self?.apply(wanted, to: device, via: connector)
                self?.applying.remove(device.guid)
            }
        }
    }

    private func apply(_ wanted: [DeviceSetting: UInt32],
                       to device: ASFWDriverConnector.FWDeviceInfo,
                       via connector: ASFWDriverConnector) async {
        let name = String(format: "%@ %@ %016llX", device.vendorName, device.modelName, device.guid)
        guard case .ready(var current) = await connector.performDeviceSettings(guid: device.guid) else {
            Self.say("\(name): settings not readable yet; will retry.")
            return
        }
        var changed = 0
        for (setting, value) in wanted.sorted(by: { $0.key.rawValue < $1.key.rawValue }) {
            guard current.supports(setting) else { continue }
            let now: UInt32
            switch setting {
            case .clockSource: now = current.clockSource
            case .opticalInputMode: now = current.opticalInputMode
            case .opticalOutputMode: now = current.opticalOutputMode
            }
            guard now != value else { continue }
            switch await connector.performDeviceSettings(guid: device.guid, set: setting, value: value) {
            case .ready(let fresh):
                current = fresh
                changed += 1
                Self.say("\(name): \(setting.title) \(now) -> \(value).")
            case .failed(let kr):
                Self.say("\(name): \(setting.title) not applied: \(connector.interpretIOReturn(kr)).")
            case .pending:
                break
            }
        }
        checkedGeneration[device.guid] = device.generation
        if changed == 0 {
            Self.say("\(name): settings already as remembered (generation \(device.generation)).")
        }
    }

    private static func say(_ line: String) {
        FileHandle.standardOutput.write(Data("[ASFW] \(line)\n".utf8))
    }
}
