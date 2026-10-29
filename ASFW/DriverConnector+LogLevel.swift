//
//  DriverConnector+LogLevel.swift
//  ASFW
//
//  The driver's log level (user-client selectors 72 and 73,
//  ASFWDriver/Logging/LogLevels.hpp): a spec such as "warn" or
//  "info irm=debug isoch=trace", and who set it. One level holds for every
//  controller, as the driver runs in one process. Shared by the app and
//  asfw-config-agent.
//

import Foundation
import IOKit

/// Who set the driver's log level (LogLevelSource in LogLevels.hpp).
enum DriverLogLevelSource: UInt32, Sendable {
    case driverDefault = 0
    case bootProperty = 1
    case app = 2
    case agent = 3
    case commandLine = 4

    var title: String {
        switch self {
        case .driverDefault: return "the driver's default"
        case .bootProperty: return "the asfw-log-level property at boot"
        case .app: return "the app"
        case .agent: return "the login agent"
        case .commandLine: return "the command line"
        }
    }
}

struct DriverLogLevel: Equatable, Sendable {
    /// The canonical spec in force, e.g. "warn" or "info irm=debug".
    let spec: String
    let source: DriverLogLevelSource

    /// The global level: the spec's first word.
    var level: String { spec.split(separator: " ").first.map(String.init) ?? spec }
    /// Whether some categories log at their own level.
    var hasOverrides: Bool { spec.contains("=") }
}

extension ASFWDriverConnector {
    /// The levels, least to most verbose. The driver logs warn by default.
    static let logLevels = ["off", "error", "warn", "info", "debug", "trace"]
    static let defaultLogLevel = "warn"

    private enum LogLevelWire {
        static let headerSize = 8 // source, reserved
        static let specCapacity = 512
    }

    /// A spec the driver would parse: a level, then optional category=level
    /// overrides, separated by spaces or commas. The driver checks the
    /// category names.
    static func isValidLogLevelSpec(_ spec: String) -> Bool {
        let words = spec.lowercased().split(whereSeparator: { $0 == " " || $0 == "," })
        guard !words.isEmpty, spec.utf8.count < LogLevelWire.specCapacity else { return false }
        let levels = Set(logLevels + ["warning"])
        return words.allSatisfy { word in
            let parts = word.split(separator: "=", omittingEmptySubsequences: false)
            switch parts.count {
            case 1: return levels.contains(String(parts[0]))
            case 2: return !parts[0].isEmpty && levels.contains(String(parts[1]))
            default: return false
            }
        }
    }

    /// The level in force, or nil without a connection.
    func logLevel() -> DriverLogLevel? {
        guard isConnected,
              let reply = transport.callStruct(selector: Method.getLogLevel.rawValue,
                                               initialCap: LogLevelWire.headerSize + LogLevelWire.specCapacity,
                                               traceCalls: false) else {
            return nil
        }
        return Self.decodeLogLevel(reply)
    }

    /// Sets the level; returns the level then in force, or nil when the driver
    /// refused the spec or there is no connection.
    func setLogLevel(_ spec: String, source: DriverLogLevelSource) -> DriverLogLevel? {
        guard isConnected, Self.isValidLogLevelSpec(spec) else { return nil }
        var request = Data(count: LogLevelWire.headerSize)
        request.withUnsafeMutableBytes { raw in
            raw.storeBytes(of: source.rawValue.littleEndian, toByteOffset: 0, as: UInt32.self)
        }
        request.append(contentsOf: Array(spec.utf8))
        request.append(0)
        guard let reply = transport.callStruct(selector: Method.setLogLevel.rawValue, input: request,
                                               initialCap: LogLevelWire.headerSize + LogLevelWire.specCapacity,
                                               traceCalls: false) else {
            log("Log level '\(spec)' not set", level: .error)
            return nil
        }
        return Self.decodeLogLevel(reply)
    }

    static func decodeLogLevel(_ data: Data) -> DriverLogLevel? {
        guard data.count > LogLevelWire.headerSize else { return nil }
        let bytes = [UInt8](data)
        let rawSource = UInt32(bytes[0]) | UInt32(bytes[1]) << 8 | UInt32(bytes[2]) << 16 | UInt32(bytes[3]) << 24
        let specBytes = bytes[LogLevelWire.headerSize...].prefix { $0 != 0 }
        guard let spec = String(bytes: specBytes, encoding: .utf8), !spec.isEmpty else { return nil }
        return DriverLogLevel(spec: spec, source: DriverLogLevelSource(rawValue: rawSource) ?? .driverDefault)
    }
}
