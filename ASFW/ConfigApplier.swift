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

import CoreAudio
import Foundation
import IOKit

@MainActor
final class ConfigApplier {
    private let store: DeviceConfigStore
    /// One connector per controller, keyed by the ASFWDriver registry entry ID.
    private var connectors: [UInt64: ASFWDriverConnector] = [:]
    /// Bus generation at which each device last had its settings checked.
    private var checkedGeneration: [UInt64: UInt32] = [:]
    private var applying = Set<UInt64>()
    /// Core Audio publishes a unit a little after discovery; a new unit's
    /// sample rate is retried this many polls before the generation is
    /// marked checked without it.
    private var rateAttempts: [UInt64: Int] = [:]
    private static let maxRateAttempts = 4
    /// A setting that did not apply (the unit re-describes after an optical
    /// change and refuses the next write for a moment) is retried on later
    /// polls this many times before the generation counts as checked.
    private var settleAttempts: [UInt64: Int] = [:]
    private static let maxSettleAttempts = 3

    /// A device's settings are checked once per bus generation, so a slow poll
    /// is enough; each pass is one driver-local call per controller.
    static let pollInterval: TimeInterval = 30

    init(store: DeviceConfigStore) {
        self.store = store
    }

    /// Runs until the process is killed; returns only if nothing can ever work.
    /// `store` is the app's own defaults inside the app, the app's container
    /// file from the asfw-config-agent tool.
    static func runForever(store: DeviceConfigStore = DeviceConfigStore()) -> Int32 {
        let applier = ConfigApplier(store: store)
        say("Device settings agent started (\(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion").map { "build \($0)" } ?? "asfw-config-agent"); settings from \(store.location)).")
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
            guard checkedGeneration[device.guid] != device.generation, !applying.contains(device.guid) else { continue }
            var wanted = store.remembered(for: device.guid)
            var sibling: ASFWDriverConnector.FWDeviceInfo?
            if wanted.isEmpty {
                // A unit the app has never configured (a third 828mkII plugged
                // into a rack of two, 2026-10-28) takes the settings of a unit
                // of the same model that is on the bus now, sample rate
                // included: a rack is used as one device.
                sibling = devices
                    .filter { $0.guid != device.guid && $0.modelName == device.modelName &&
                              !store.remembered(for: $0.guid).isEmpty }
                    .sorted { $0.guid < $1.guid }
                    .first
                if let sibling { wanted = store.remembered(for: sibling.guid) }
            }
            guard !wanted.isEmpty else { continue }
            applying.insert(device.guid)
            Task { [weak self] in
                await self?.apply(wanted, to: device, via: connector, inheritedFrom: sibling)
                self?.applying.remove(device.guid)
            }
        }
    }

    private func apply(_ wanted: [DeviceSetting: UInt32],
                       to device: ASFWDriverConnector.FWDeviceInfo,
                       via connector: ASFWDriverConnector,
                       inheritedFrom sibling: ASFWDriverConnector.FWDeviceInfo? = nil) async {
        let name = String(format: "%@ (%016llX)", device.displayLabel, device.guid)
        guard case .ready(var current) = await connector.performDeviceSettings(guid: device.guid) else {
            Self.say("\(name): settings not readable yet; will retry.")
            return
        }
        if let sibling {
            Self.say("\(name): no remembered settings; taking those of \(sibling.displayLabel).")
        }
        var changed = 0
        var unsettled = 0
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
                unsettled += 1
                Self.say("\(name): \(setting.title) not applied: \(connector.interpretIOReturn(kr)).")
            case .pending:
                unsettled += 1
            }
        }
        if unsettled > 0 {
            let attempts = (settleAttempts[device.guid] ?? 0) + 1
            settleAttempts[device.guid] = attempts
            if attempts < Self.maxSettleAttempts {
                Self.say("\(name): \(unsettled) setting(s) not applied yet; will retry.")
                return
            }
            Self.say("\(name): \(unsettled) setting(s) left unapplied after \(attempts) attempts.")
        }
        settleAttempts.removeValue(forKey: device.guid)
        if let sibling, !matchSampleRate(of: device, to: sibling, name: name) {
            // Core Audio has not published the unit yet: leave the generation
            // unchecked so the next poll tries the rate again, a few times.
            let attempts = (rateAttempts[device.guid] ?? 0) + 1
            rateAttempts[device.guid] = attempts
            if attempts < Self.maxRateAttempts { return }
            Self.say("\(name): sample rate left as is after \(attempts) attempts.")
        }
        rateAttempts.removeValue(forKey: device.guid)
        checkedGeneration[device.guid] = device.generation
        if changed == 0 {
            Self.say("\(name): settings already as remembered (generation \(device.generation)).")
        }
    }

    /// Set the new unit's Core Audio nominal sample rate to the sibling's.
    /// Returns false when either device is not published to Core Audio yet.
    private func matchSampleRate(of device: ASFWDriverConnector.FWDeviceInfo,
                                 to sibling: ASFWDriverConnector.FWDeviceInfo,
                                 name: String) -> Bool {
        guard let siblingID = CoreAudioDevice.find(guid: sibling.guid),
              let deviceID = CoreAudioDevice.find(guid: device.guid),
              let rate = CoreAudioDevice.nominalSampleRate(siblingID) else {
            return false
        }
        guard CoreAudioDevice.nominalSampleRate(deviceID) != rate else { return true }
        let status = CoreAudioDevice.setNominalSampleRate(deviceID, rate)
        if status == noErr {
            Self.say("\(name): sample rate -> \(Int(rate)) Hz, as \(sibling.displayLabel).")
        } else {
            Self.say("\(name): sample rate \(Int(rate)) Hz not applied (OSStatus \(status)).")
        }
        return true
    }

    private static func say(_ line: String) {
        FileHandle.standardOutput.write(Data("[ASFW] \(line)\n".utf8))
    }
}

/// The Core Audio side of a published unit (device UID `ASFW-<GUID>`, as the
/// driver names it).
private enum CoreAudioDevice {
    static func find(guid: UInt64) -> AudioObjectID? {
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var uid = String(format: "ASFW-%016llX", guid) as CFString
        var deviceID = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let status = withUnsafeMutablePointer(to: &uid) { uidPointer in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address,
                                       UInt32(MemoryLayout<CFString>.size), uidPointer, &size, &deviceID)
        }
        return status == noErr && deviceID != kAudioObjectUnknown ? deviceID : nil
    }

    static func nominalSampleRate(_ deviceID: AudioObjectID) -> Double? {
        var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var rate = 0.0
        var size = UInt32(MemoryLayout<Double>.size)
        let status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &rate)
        return status == noErr && rate > 0 ? rate : nil
    }

    static func setNominalSampleRate(_ deviceID: AudioObjectID, _ rate: Double) -> OSStatus {
        var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var value = rate
        return AudioObjectSetPropertyData(deviceID, &address, 0, nil, UInt32(MemoryLayout<Double>.size), &value)
    }
}
