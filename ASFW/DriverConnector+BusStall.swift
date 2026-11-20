//
//  DriverConnector+BusStall.swift
//  ASFW
//
//  The CPU idle limit the driver holds while a device runs IO (user-client
//  selectors 76 and 77, ASFWDriver/Audio/DriverKit/Runtime/BusStallLimit.hpp):
//  the longest wake-up from an idle state the system may then choose, in
//  microseconds, or off. One limit holds for every controller, as the driver
//  runs in one process; a new one holds from each device's next IO start.
//  Shared by the app's agent loop and asfw-config-agent.
//

import Foundation
import IOKit

struct DriverBusStall: Equatable, Sendable {
    /// The limit set, in nanoseconds; 0 is off.
    let limitNanos: UInt32
    /// How many devices run IO with a limit held now.
    let holders: UInt32

    /// "off", or the limit in microseconds: "5".
    var spec: String { limitNanos == 0 ? "off" : String(limitNanos / 1000) }
}

extension ASFWDriverConnector {
    /// What the driver takes: off, or microseconds (DriverKit's kIOMaxBusStall
    /// values). The driver holds 5 by default.
    static let busStallSpecs = ["off", "5", "10", "20", "25", "30", "40"]
    static let defaultBusStall = "5"

    /// Nanoseconds for a spec, nil for anything but busStallSpecs.
    static func busStallNanos(_ spec: String) -> UInt32? {
        let word = spec.lowercased()
        guard busStallSpecs.contains(word) else { return nil }
        return word == "off" ? 0 : UInt32(word).map { $0 * 1000 }
    }

    /// The limit set, or nil without a connection.
    func busStall() -> DriverBusStall? {
        guard isConnected,
              let reply = transport.callScalarStruct(selector: Method.getBusStallLimit.rawValue,
                                                     scalars: [], initialCap: 16) else {
            return nil
        }
        return Self.decodeBusStall(reply)
    }

    /// Sets the limit; returns the state then, or nil when the driver refused
    /// the value or there is no connection.
    func setBusStall(nanos: UInt32) -> DriverBusStall? {
        guard isConnected,
              let reply = transport.callScalarStruct(selector: Method.setBusStallLimit.rawValue,
                                                     scalars: [UInt64(nanos)], initialCap: 16) else {
            return nil
        }
        return Self.decodeBusStall(reply)
    }

    /// BusStallWire: limitNanos, holders, little-endian.
    static func decodeBusStall(_ data: Data) -> DriverBusStall? {
        guard data.count >= 8 else { return nil }
        let bytes = [UInt8](data.prefix(8))
        func word(_ at: Int) -> UInt32 {
            UInt32(bytes[at]) | UInt32(bytes[at + 1]) << 8 | UInt32(bytes[at + 2]) << 16 | UInt32(bytes[at + 3]) << 24
        }
        return DriverBusStall(limitNanos: word(0), holders: word(4))
    }
}
