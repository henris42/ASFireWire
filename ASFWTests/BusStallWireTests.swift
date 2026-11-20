//
//  BusStallWireTests.swift
//  ASFWTests
//
//  Decodes BusStallWire (ASFWDriver/Audio/DriverKit/Runtime/BusStallLimit.hpp,
//  user-client selectors 76/77) and checks the limits asfw-config-agent sends.
//

import Foundation
import Testing
@testable import ASFW

struct BusStallWireTests {
    /// limitNanos, holders.
    private func wire(limitNanos: UInt32, holders: UInt32) -> Data {
        var data = Data()
        withUnsafeBytes(of: limitNanos.littleEndian) { data.append(contentsOf: $0) }
        withUnsafeBytes(of: holders.littleEndian) { data.append(contentsOf: $0) }
        return data
    }

    @Test func decodesTheLimitAndTheHolders() throws {
        let held = try #require(ASFWDriverConnector.decodeBusStall(wire(limitNanos: 5000, holders: 2)))
        #expect(held.limitNanos == 5000)
        #expect(held.holders == 2)
        #expect(held.spec == "5")

        let off = try #require(ASFWDriverConnector.decodeBusStall(wire(limitNanos: 0, holders: 0)))
        #expect(off.spec == "off")
        #expect(ASFWDriverConnector.decodeBusStall(Data(count: 7)) == nil)
    }

    @Test func takesOnlyTheDriversLimits() {
        #expect(ASFWDriverConnector.busStallNanos("off") == 0)
        #expect(ASFWDriverConnector.busStallNanos("OFF") == 0)
        #expect(ASFWDriverConnector.busStallNanos("5") == 5000)
        #expect(ASFWDriverConnector.busStallNanos("40") == 40000)
        #expect(ASFWDriverConnector.busStallNanos(ASFWDriverConnector.defaultBusStall) == 5000)
        for spec in ["", "0", "15", "50", "5us", "5000", "default"] {
            #expect(ASFWDriverConnector.busStallNanos(spec) == nil, "\(spec)")
        }
    }
}
