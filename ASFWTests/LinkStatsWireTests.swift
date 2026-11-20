//
//  LinkStatsWireTests.swift
//  ASFWTests
//
//  Decodes LinkStatsWire (ASFWDriver/Diagnostics/LinkStatsWire.hpp, user-client
//  selector 78) and checks the report asfw-config-agent --link-stats prints.
//

import Foundation
import Testing
@testable import ASFW

struct LinkStatsWireTests {
    private func wire(_ words: [UInt64]) -> Data {
        var data = Data()
        for word in words {
            withUnsafeBytes(of: word.littleEndian) { data.append(contentsOf: $0) }
        }
        return data
    }

    /// One unit, one node, two PHY ports.
    private var sample: [UInt64] {
        var words: [UInt64] = [1, 1, 1, 2, 0]
        words += [3, 0, 0, 0, 0, 0, 2, 5]
        words += [0x0001_F200_0001_F968, 3, 2, 480_000, 1, 0, 0, 2, 16, 4, 0, 0x001D, 480_000, 0, 0, 0]
        words += [1, 40, 0, 6, 0, 0, 0]
        words += [0 | 0x0E << 8 | 0x40 << 16, 1 | 0x00 << 8]
        return words
    }

    @Test func decodesEveryPart() throws {
        let stats = try #require(DriverLinkStats.decode(wire(sample)))
        #expect(stats.controller.cycleLost == 3)
        #expect(stats.controller.busResets == 2)
        #expect(stats.controller.generation == 5)
        #expect(stats.units.count == 1)
        let unit = stats.units[0]
        #expect(unit.guid == 0x0001_F200_0001_F968)
        #expect(unit.rxRuns && unit.txRuns && !unit.dbcNotChecked)
        #expect(unit.rxPackets == 480_000)
        #expect(unit.rxCrc == 1)
        #expect(unit.dbcGaps == 2)
        #expect(unit.dbcBlocks == 16)
        #expect(unit.rxCyclesMissed == 4)
        #expect(unit.rxLastBadStatus == 0x1D)
        #expect(stats.asyncNodes == [.init(node: 1, requests: 40, ackErr: 0, busy: 6, noAck: 0, timeouts: 0, other: 0)])
        #expect(stats.phyPorts.count == 2)
        #expect(stats.phyPorts[0].connected && stats.phyPorts[0].bias && stats.phyPorts[0].child)
        #expect(stats.phyPorts[0].speedCode == 2)
        #expect(!stats.phyPorts[1].connected)
        #expect(stats.phyState == .read)
        // cycleLost 3, crc 1, dbc gaps 2, cycles missed 4; busy and bus resets are not errors.
        #expect(stats.errors == 10)
    }

    @Test func refusesWhatIsNotTheWire() {
        #expect(DriverLinkStats.decode(Data()) == nil)
        #expect(DriverLinkStats.decode(wire(Array(sample.dropLast()))) == nil)
        var other = sample
        other[0] = 2
        #expect(DriverLinkStats.decode(wire(other)) == nil)
    }

    @Test func printsTheUnitAndTheVerdict() throws {
        let stats = try #require(DriverLinkStats.decode(wire(sample)))
        let text = stats.lines.joined(separator: "\n")
        #expect(text.contains("0x0001f2000001f968, streaming"))
        #expect(text.contains("rx 480000 pk S400, crc 1, other status 0 (last 0x001d)"))
        #expect(text.contains("dbc gaps 2 (16 blocks)"))
        #expect(text.contains("PHY port 0: connected, child, bias, negotiated S400"))
        #expect(text.contains("PHY port 1: nothing connected"))
        #expect(text.contains("Error counters above zero: 10"))

        var quiet = DriverLinkStats()
        quiet.phyState = .skippedStreaming
        let quietText = quiet.lines.joined(separator: "\n")
        #expect(quietText.contains("No error counter above zero."))
        #expect(quietText.contains("not read while a stream runs"))
    }
}
