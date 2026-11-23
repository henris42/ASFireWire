//
//  DeviceSettingsWireTests.swift
//  ASFWTests
//
//  Decodes DeviceSettingsResultWire (ASFWDriver/UserClient/WireFormats/
//  DeviceSettingsWire.hpp): 48 bytes from a driver without the analog input
//  switches, 60 with them.
//

import Foundation
import Testing
@testable import ASFW

struct DeviceSettingsWireTests {
    /// Little-endian quadlets, as the driver writes the packed structure.
    private func wire(_ words: [UInt32]) -> Data {
        words.reduce(into: Data()) { data, word in
            withUnsafeBytes(of: word.littleEndian) { data.append(contentsOf: $0) }
        }
    }

    /// version, status, guid (2 words), supported settings, clock source and
    /// options, optical in/out modes and options, clock and optical registers.
    private func baseWords(supported: UInt32) -> [UInt32] {
        [1, 0, 0x0000_0001, 0x0001_F200, supported, 4, 0x17, 0, 0, 0x7, 0x0000_000C, 0x0000_0000]
    }

    @Test func aResultWithoutInputSwitchesDecodesAsBefore() throws {
        let snapshot = try #require(DeviceSettingsSnapshot(wire: wire(baseWords(supported: 0b1110))))
        #expect(snapshot.clockSource == 4)
        #expect(snapshot.clockRaw == 0x0C)
        #expect(snapshot.analogInputSwitches == 0)
        #expect(snapshot.switchableInputs.isEmpty)
        #expect(!snapshot.supports(.inputLevels))
        #expect(!snapshot.supports(.inputBoosts))
    }

    @Test func theExtendedResultCarriesTheInputSwitches() throws {
        let supported: UInt32 = 0b11_1110
        let data = wire(baseWords(supported: supported) + [0x0000_00FF, 0xA5A5_0003, 0x0000_0080])
        let snapshot = try #require(DeviceSettingsSnapshot(wire: data))
        #expect(snapshot.supports(.inputLevels))
        #expect(snapshot.supports(.inputBoosts))
        #expect(snapshot.switchableInputs == Array(1...8))
        // Only the switchable inputs' bits are their state.
        #expect(snapshot.inputLevels == 0x03)
        #expect(snapshot.inputBoosts == 0x80)
        #expect(snapshot.inputLevelRaw == 0xA5A5_0003)
        // 60 bytes: a driver without the monitor mixer field.
        #expect(snapshot.hardwareMonitoring == 0)
    }

    @Test func offIsTheCoaxChoiceWhereTheOpticalPortCanCarrySpdif() {
        // 828mkII, Traveler: S/PDIF is on coax unless the optical port takes it.
        let withSpdif: [DeviceOpticalMode] = [.off, .adat, .spdif]
        #expect(DeviceOpticalMode.off.title(among: withSpdif) == "Coax (S/PDIF)")
        #expect(DeviceOpticalMode.adat.title(among: withSpdif) == "ADAT")
        // 8pre: the optical port is ADAT or off.
        #expect(DeviceOpticalMode.off.title(among: [.off, .adat]) == "Off")
    }

    @Test func theMixerResultCarriesHardwareMonitoring() throws {
        let supported: UInt32 = 0b111_1110
        let data = wire(baseWords(supported: supported) + [0x0000_00FF, 0, 0, 1])
        let snapshot = try #require(DeviceSettingsSnapshot(wire: data))
        #expect(data.count == DeviceSettingsSnapshot.mixerWireSize)
        #expect(snapshot.supports(.hardwareMonitoring))
        #expect(snapshot.hardwareMonitoring == 1)
    }

    @Test func aTravelerOffersAnalog5To8() throws {
        let data = wire(baseWords(supported: 0b11_1110) + [0x0000_00F0, 0x0000_001F, 0])
        let snapshot = try #require(DeviceSettingsSnapshot(wire: data))
        #expect(snapshot.switchableInputs == [5, 6, 7, 8])
        #expect(snapshot.inputLevels == 0x10)
    }

    @Test func aMaskChangesOneInputOnly() {
        #expect(DeviceSettingsSnapshot.mask(0x03, input: 2, set: false) == 0x01)
        #expect(DeviceSettingsSnapshot.mask(0x01, input: 8, set: true) == 0x81)
        #expect(DeviceSettingsSnapshot.mask(0x81, input: 8, set: true) == 0x81)
        #expect(DeviceSettingsSnapshot.inputs(in: 0x81) == [1, 8])
    }

    @Test func anotherVersionIsRefused() {
        var words = baseWords(supported: 0)
        words[0] = 2
        #expect(DeviceSettingsSnapshot(wire: wire(words)) == nil)
    }
}
