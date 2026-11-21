//
//  ConfigApplier.swift
//  ASFW
//
//  The login agent's loop (asfw-config-agent, run from
//  /Library/LaunchAgents/net.asfw.apply-config.plist). It watches every
//  FireWire controller the driver claims, and whenever a device with
//  remembered settings (DeviceConfigStore) appears or comes back with a new
//  bus generation, reads the device's current settings and writes the ones
//  that differ. It sets the remembered driver log level and CPU idle limit on
//  each new driver instance, and saves the driver's start steps to
//  ~/Library/Logs/ASFW (StartTraceLog). The app itself applies a change
//  immediately; this makes it hold across reboots, re-plugs and power cycles
//  of the device.
//

import Combine
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
    /// The remembered log level last handled per controller (registry entry
    /// ID); "" when nothing is remembered.
    private var levelHandled: [UInt64: String] = [:]
    /// The same for the remembered CPU idle limit.
    private var stallHandled: [UInt64: String] = [:]
    private let startedAt = Date()
    /// IOKit notifications for driver instances coming and going, so a new
    /// one gets its log level at once rather than at the next poll.
    private let registry = CardRegistry()
    private var registryWatch: AnyCancellable?

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
        say("Driver log level to set: \(applier.store.logLevel ?? "none remembered (the driver's default, warn)").")
        say("CPU idle limit to set: \(applier.store.busStall ?? "none remembered (the driver's default, 5 us)").")
        StartTraceLog.saveBoot { say($0) }
        applier.registryWatch = applier.registry.$cards.dropFirst().sink { [weak applier] _ in
            applier?.reconcileControllers()
        }
        applier.poll()
        while true {
            _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.5))
        }
    }

    private func poll() {
        reconcileControllers()
        for (id, connector) in connectors where connector.isConnected {
            applyLogLevel(to: connector, id: id)
            applyBusStall(to: connector, id: id)
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
            let connector = ASFWDriverConnector(registryEntryID: card.registryEntryID, pollsStatus: false)
            connector.connect(forceAttempt: true)
            connectors[card.registryEntryID] = connector
            Self.say("Watching \(card.displayName).")
            newDriverInstance(card.registryEntryID, via: connector)
        }
    }

    /// A driver instance the agent has not seen: its log level once the
    /// connection is up, and, when it started after login, its start steps.
    private func newDriverInstance(_ id: UInt64, via connector: ASFWDriverConnector, attempt: Int = 0) {
        let seen = Date()
        if attempt == 0 && seen.timeIntervalSince(startedAt) > 30 {
            // Bring-up ends with the first ROM scan, a few seconds after Start.
            DispatchQueue.main.asyncAfter(deadline: .now() + 20) {
                StartTraceLog.appendStart(since: seen.addingTimeInterval(-60)) { Self.say($0) }
            }
        }
        guard connector.isConnected else {
            if attempt < 20 {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    guard let self, self.connectors[id] === connector else { return }
                    self.newDriverInstance(id, via: connector, attempt: attempt + 1)
                }
            }
            return
        }
        applyLogLevel(to: connector, id: id)
        applyBusStall(to: connector, id: id)
    }

    /// Sets the remembered log level unless it is in force already, or the
    /// controller's asfw-log-level property set the level at boot (that one
    /// stays until the app or the command line sets another).
    private func applyLogLevel(to connector: ASFWDriverConnector, id: UInt64) {
        let wanted = store.logLevel ?? ""
        guard levelHandled[id] != wanted else { return }
        guard !wanted.isEmpty else {
            levelHandled[id] = wanted
            return
        }
        guard let current = connector.logLevel() else { return }
        levelHandled[id] = wanted
        if current.source == .bootProperty {
            Self.say("Log level \(current.spec) was set at boot by asfw-log-level; not setting \(wanted).")
            return
        }
        guard let applied = connector.setLogLevel(wanted, source: .agent) else {
            Self.say("The driver did not take the remembered log level '\(wanted)'.")
            return
        }
        if applied.spec != current.spec {
            Self.say("Log level \(current.spec) -> \(applied.spec).")
        }
        // The command line sets the driver and then saves: a level saved while
        // this one was being set is newer than what the driver now has.
        if (store.logLevel ?? "") != wanted {
            Self.say("The saved log level changed meanwhile; setting it again.")
            applyLogLevel(to: connector, id: id)
        }
    }

    /// Sets the remembered CPU idle limit (DriverConnector+BusStall.swift)
    /// unless it is the one set already. A driver without the selector
    /// (before b226) is asked once.
    private func applyBusStall(to connector: ASFWDriverConnector, id: UInt64) {
        let wanted = store.busStall ?? ""
        guard stallHandled[id] != wanted else { return }
        stallHandled[id] = wanted
        guard !wanted.isEmpty else { return }
        guard let nanos = ASFWDriverConnector.busStallNanos(wanted) else {
            Self.say("The remembered CPU idle limit '\(wanted)' is not one (off, 5, 10, 20, 25, 30 or 40); ignored.")
            return
        }
        guard let current = connector.busStall() else { return }
        guard current.limitNanos != nanos else { return }
        guard let applied = connector.setBusStall(nanos: nanos) else {
            Self.say("The driver did not take the remembered CPU idle limit '\(wanted)'.")
            return
        }
        Self.say("CPU idle limit \(current.spec) -> \(applied.spec), from each device's next IO start.")
    }

    private func checkDevices(on connector: ASFWDriverConnector) {
        guard let devices = connector.getDiscoveredDevices() else { return }
        for device in devices where device.vendorId == ASFWDriverConnector.motuVendorID {
            guard checkedGeneration[device.guid] != device.generation, !applying.contains(device.guid) else { continue }
            var wanted = store.remembered(for: device.guid)
            var sibling: ASFWDriverConnector.FWDeviceInfo?
            if wanted.isEmpty && store.inheritsSiblingSettings {
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
            var target = value
            switch setting {
            case .clockSource: now = current.clockSource
            case .opticalInputMode: now = current.opticalInputMode
            case .opticalOutputMode: now = current.opticalOutputMode
            case .inputLevels:
                now = current.inputLevels
                target = value & current.analogInputSwitches
            case .inputBoosts:
                now = current.inputBoosts
                target = value & current.analogInputSwitches
            }
            guard now != target else { continue }
            switch await connector.performDeviceSettings(guid: device.guid, set: setting, value: target) {
            case .ready(let fresh):
                current = fresh
                changed += 1
                Self.say("\(name): \(setting.title) \(Self.shown(now, setting)) -> \(Self.shown(target, setting)).")
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

    /// A value as the log shows it: the input switch settings are bit masks.
    private static func shown(_ value: UInt32, _ setting: DeviceSetting) -> String {
        switch setting {
        case .inputLevels, .inputBoosts: return String(format: "0x%02X", value)
        case .clockSource, .opticalInputMode, .opticalOutputMode: return "\(value)"
        }
    }

    nonisolated static func say(_ line: String) {
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
