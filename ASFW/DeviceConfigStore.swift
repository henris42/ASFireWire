//
//  DeviceConfigStore.swift
//  ASFW
//
//  Remembered device settings (clock source, optical modes) per device GUID.
//  Kept in the app's UserDefaults, which the sandbox allows and which the
//  login agent (`ASFW --apply-config`, same bundle) shares, so a choice made in
//  the app is applied again after every login and whenever the device
//  reappears. Keyed by GUID, not by controller: a device keeps its settings
//  whichever FireWire card it is plugged into.
//

import Foundation

struct DeviceConfigStore {
    static let defaultsKey = "net.asfw.deviceSettings"

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// GUID (hex) -> setting raw value (as string key) -> value.
    private func table() -> [String: [String: Int]] {
        defaults.dictionary(forKey: Self.defaultsKey) as? [String: [String: Int]] ?? [:]
    }

    private static func key(_ guid: UInt64) -> String {
        String(format: "%016llX", guid)
    }

    func remembered(for guid: UInt64) -> [DeviceSetting: UInt32] {
        var result: [DeviceSetting: UInt32] = [:]
        for (rawKey, value) in table()[Self.key(guid)] ?? [:] {
            if let raw = UInt32(rawKey), let setting = DeviceSetting(rawValue: raw), value >= 0 {
                result[setting] = UInt32(value)
            }
        }
        return result
    }

    func remember(guid: UInt64, setting: DeviceSetting, value: UInt32) {
        var all = table()
        var entry = all[Self.key(guid)] ?? [:]
        entry[String(setting.rawValue)] = Int(value)
        all[Self.key(guid)] = entry
        defaults.set(all, forKey: Self.defaultsKey)
    }

    func forget(guid: UInt64) {
        var all = table()
        all.removeValue(forKey: Self.key(guid))
        defaults.set(all, forKey: Self.defaultsKey)
    }

    /// Every device with remembered settings.
    func rememberedGUIDs() -> [UInt64] {
        table().keys.compactMap { UInt64($0, radix: 16) }.sorted()
    }
}
