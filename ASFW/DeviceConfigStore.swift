//
//  DeviceConfigStore.swift
//  ASFW
//
//  Remembered device settings (clock source, optical modes, analog input
//  switches) per device GUID, and the driver log level.
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
    /// The sandboxed app keeps its defaults in its container; a process outside
    /// the sandbox (asfw-config-agent) reads that file directly.
    private let fileURL: URL?
    /// Both of the app's preference files, for the log level (see logLevel).
    private let candidateURLs: [URL]

    static let appBundleID = "net.asfw.app"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.fileURL = nil
        self.candidateURLs = []
    }

    /// Read-only view of the app's preferences from another process. A
    /// sandboxed app keeps them in its container; an unsandboxed one (the
    /// local-install builds, see tools/build-for-macos.sh) in
    /// ~/Library/Preferences. Whichever file holds settings wins.
    static func readingAppContainer() -> DeviceConfigStore {
        let home = FileManager.default.homeDirectoryForCurrentUser
        let candidates = [
            home.appendingPathComponent("Library/Preferences/\(appBundleID).plist"),
            home.appendingPathComponent("Library/Containers/\(appBundleID)/Data/Library/Preferences/\(appBundleID).plist"),
        ]
        for url in candidates where !DeviceConfigStore(fileURL: url, candidates: []).table().isEmpty {
            return DeviceConfigStore(fileURL: url, candidates: candidates)
        }
        return DeviceConfigStore(fileURL: candidates[0], candidates: candidates)
    }

    private init(fileURL: URL, candidates: [URL]) {
        self.defaults = .standard
        self.fileURL = fileURL
        self.candidateURLs = candidates
    }

    var location: String { fileURL?.path ?? "UserDefaults \(Bundle.main.bundleIdentifier ?? "?")" }

    /// GUID (hex) -> setting raw value (as string key) -> value.
    private func table() -> [String: [String: Int]] {
        if let fileURL {
            guard let data = try? Data(contentsOf: fileURL),
                  let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] else {
                return [:]
            }
            return plist[Self.defaultsKey] as? [String: [String: Int]] ?? [:]
        }
        return defaults.dictionary(forKey: Self.defaultsKey) as? [String: [String: Int]] ?? [:]
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
        guard fileURL == nil else { return }
        var all = table()
        var entry = all[Self.key(guid)] ?? [:]
        entry[String(setting.rawValue)] = Int(value)
        all[Self.key(guid)] = entry
        defaults.set(all, forKey: Self.defaultsKey)
    }

    func forget(guid: UInt64) {
        guard fileURL == nil else { return }
        var all = table()
        all.removeValue(forKey: Self.key(guid))
        defaults.set(all, forKey: Self.defaultsKey)
    }

    /// Whether a MOTU unit the app never configured takes a same-model
    /// sibling's settings and sample rate (the agent's seeding, off unless the
    /// user turns it on: `defaults write net.asfw.app inheritSiblingSettings -bool YES`).
    static let inheritSiblingSettingsKey = "inheritSiblingSettings"
    var inheritsSiblingSettings: Bool {
        if let fileURL {
            guard let data = try? Data(contentsOf: fileURL),
                  let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] else {
                return false
            }
            return plist[Self.inheritSiblingSettingsKey] as? Bool ?? false
        }
        return defaults.bool(forKey: Self.inheritSiblingSettingsKey)
    }

    /// Every device with remembered settings.
    func rememberedGUIDs() -> [UInt64] {
        table().keys.compactMap { UInt64($0, radix: 16) }.sorted()
    }

    /// The driver log level the login agent sets after each driver start: a
    /// spec such as "debug" or "info irm=debug", saved by the app, by
    /// `asfw-config-agent --log-level`, or by
    /// `defaults write net.asfw.app logLevel -string debug`. nil: the driver's
    /// default. From another process both preference files are read and the
    /// newer one that has the key wins, since `defaults write` and the agent
    /// write ~/Library/Preferences while a sandboxed app writes its container.
    static let logLevelKey = "logLevel"
    var logLevel: String? {
        guard fileURL != nil else {
            return defaults.string(forKey: Self.logLevelKey)
        }
        var newest: (date: Date, spec: String)?
        for url in candidateURLs {
            guard let data = try? Data(contentsOf: url),
                  let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
                  let spec = plist[Self.logLevelKey] as? String, !spec.isEmpty else { continue }
            let modified = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?
                .contentModificationDate ?? .distantPast
            if newest == nil || modified > newest!.date {
                newest = (modified, spec)
            }
        }
        return newest?.spec
    }

    /// The app saves the level it set (nil forgets it).
    func rememberLogLevel(_ spec: String?) {
        guard fileURL == nil else { return }
        defaults.set(spec, forKey: Self.logLevelKey)
    }

    /// The agent's command line saves to the app's domain in
    /// ~/Library/Preferences, which it reads back as one of its candidates.
    static func saveLogLevelForApp(_ spec: String?) {
        CFPreferencesSetAppValue(logLevelKey as CFString, spec as CFString?, appBundleID as CFString)
        CFPreferencesAppSynchronize(appBundleID as CFString)
    }
}
