//
//  LogLevelWireTests.swift
//  ASFWTests
//
//  Decodes LogLevelWire (ASFWDriver/Logging/LogLevels.hpp, user-client
//  selectors 72/73) and checks the spec the app and asfw-config-agent send.
//

import Foundation
import Testing
@testable import ASFW

struct LogLevelWireTests {
    /// source, reserved, then the NUL-terminated spec in a 512-byte field.
    private func wire(source: UInt32, spec: String) -> Data {
        var data = Data()
        withUnsafeBytes(of: source.littleEndian) { data.append(contentsOf: $0) }
        data.append(contentsOf: [0, 0, 0, 0])
        var field = Array(spec.utf8)
        field += [UInt8](repeating: 0, count: 512 - field.count)
        data.append(contentsOf: field)
        return data
    }

    @Test func decodesTheLevelInForceAndWhoSetIt() throws {
        let state = try #require(ASFWDriverConnector.decodeLogLevel(wire(source: 1, spec: "info irm=debug")))
        #expect(state.spec == "info irm=debug")
        #expect(state.level == "info")
        #expect(state.hasOverrides)
        #expect(state.source == .bootProperty)

        let plain = try #require(ASFWDriverConnector.decodeLogLevel(wire(source: 0, spec: "warn")))
        #expect(plain.level == "warn")
        #expect(!plain.hasOverrides)
        #expect(plain.source == .driverDefault)
    }

    @Test func refusesAReplyWithoutASpec() {
        #expect(ASFWDriverConnector.decodeLogLevel(Data(count: 8)) == nil)
        #expect(ASFWDriverConnector.decodeLogLevel(wire(source: 2, spec: "")) == nil)
    }

    @Test func acceptsTheSpecsTheDriverParses() {
        for spec in ["warn", "TRACE", "info irm=debug isoch=trace", "level=info,irm=debug", "warning"] {
            #expect(ASFWDriverConnector.isValidLogLevelSpec(spec), "\(spec)")
        }
        for spec in ["", "loud", "irm=", "=debug", "irm=loud", "a=b=c"] {
            #expect(!ASFWDriverConnector.isValidLogLevelSpec(spec), "\(spec)")
        }
    }
}
