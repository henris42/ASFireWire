//
//  DriverConnector+LinkStats.swift
//  ASFW
//
//  The driver's link metrics as totals (user-client selector 78,
//  ASFWDriver/Diagnostics/LinkStatsWire.hpp): what the controller, the
//  isochronous packets of each streaming unit and the asynchronous requests to
//  each node have counted of errors, and the local PHY's port status. The
//  same counters feed the driver's `[Link]` log lines. A unit's counters start
//  with its stream; the controller's and the nodes' with the driver.
//  Used by asfw-config-agent --link-stats.
//

import Foundation
import IOKit

struct DriverLinkStats: Equatable, Sendable {
    struct Controller: Equatable, Sendable {
        var cycleLost: UInt64 = 0
        var cycleInconsistent: UInt64 = 0
        var cycleTooLong: UInt64 = 0
        var postedWriteErr: UInt64 = 0
        var regAccessFail: UInt64 = 0
        var unrecoverableError: UInt64 = 0
        var busResets: UInt64 = 0
        var generation: UInt64 = 0

        /// Bus resets are listed, not counted as errors: every plug and every start makes some.
        var errors: UInt64 {
            cycleLost + cycleInconsistent + cycleTooLong + postedWriteErr + regAccessFail + unrecoverableError
        }
    }

    struct Unit: Equatable, Sendable {
        var guid: UInt64 = 0
        var rxRuns = false
        var txRuns = false
        var dbcNotChecked = false
        var rxSpeedCode: UInt64 = 0
        var rxPackets: UInt64 = 0
        var rxCrc: UInt64 = 0
        var rxOtherStatus: UInt64 = 0
        var rxLength: UInt64 = 0
        var dbcGaps: UInt64 = 0
        var dbcBlocks: UInt64 = 0
        var rxCyclesMissed: UInt64 = 0
        var cipChanges: UInt64 = 0
        var rxLastBadStatus: UInt64 = 0
        var txPackets: UInt64 = 0
        var txStatusErrors: UInt64 = 0
        var txCyclesMissed: UInt64 = 0
        var txLastBadStatus: UInt64 = 0

        var errors: UInt64 {
            rxCrc + rxOtherStatus + rxLength + dbcGaps + rxCyclesMissed + cipChanges + txStatusErrors + txCyclesMissed
        }
    }

    struct AsyncNode: Equatable, Sendable {
        var node: UInt64 = 0
        var requests: UInt64 = 0
        var ackErr: UInt64 = 0
        var busy: UInt64 = 0
        var noAck: UInt64 = 0
        var timeouts: UInt64 = 0
        var other: UInt64 = 0

        /// A node answering busy is the node's doing, not the link's.
        var errors: UInt64 { ackErr + noAck + timeouts + other }
    }

    struct PhyPort: Equatable, Sendable {
        var port: UInt8 = 0
        /// PHY registers 8 to 11 with this port selected (page 0).
        var registers: [UInt8] = [0, 0, 0, 0]

        var connected: Bool { registers[0] & 0x04 != 0 }
        var bias: Bool { registers[0] & 0x02 != 0 }
        var disabled: Bool { registers[0] & 0x01 != 0 }
        var child: Bool { registers[0] & 0x08 != 0 }
        var fault: Bool { registers[1] & 0x08 != 0 }
        var speedCode: UInt8 { registers[1] >> 5 }
    }

    enum PhyState: UInt64, Sendable {
        case read = 0
        case skippedStreaming = 1
        case unreadable = 2
    }

    var controller = Controller()
    var units: [Unit] = []
    var asyncNodes: [AsyncNode] = []
    var phyPorts: [PhyPort] = []
    var phyState: PhyState = .read

    /// Every counter that should stay at zero, added up.
    var errors: UInt64 {
        controller.errors + units.reduce(0) { $0 + $1.errors } + asyncNodes.reduce(0) { $0 + $1.errors }
    }

    static let version: UInt64 = 1
    private static let headerWords = 5
    private static let controllerWords = 8
    private static let unitWords = 16
    private static let asyncWords = 7

    /// LinkStatsWire: 64-bit little-endian words; nil for anything else.
    static func decode(_ data: Data) -> DriverLinkStats? {
        let bytes = [UInt8](data)
        let count = bytes.count / 8
        guard count >= headerWords + controllerWords else { return nil }
        func word(_ index: Int) -> UInt64 {
            (0..<8).reduce(UInt64(0)) { $0 | UInt64(bytes[index * 8 + $1]) << UInt64($1 * 8) }
        }
        guard word(0) == version else { return nil }
        let unitCount = Int(clamping: word(1))
        let asyncCount = Int(clamping: word(2))
        let portCount = Int(clamping: word(3))
        guard unitCount <= 64, asyncCount <= 64, portCount <= 64,
              count >= headerWords + controllerWords + unitCount * unitWords + asyncCount * asyncWords + portCount else {
            return nil
        }

        var stats = DriverLinkStats()
        stats.phyState = PhyState(rawValue: word(4)) ?? .unreadable
        var at = headerWords
        stats.controller = Controller(cycleLost: word(at), cycleInconsistent: word(at + 1), cycleTooLong: word(at + 2),
                                      postedWriteErr: word(at + 3), regAccessFail: word(at + 4),
                                      unrecoverableError: word(at + 5), busResets: word(at + 6),
                                      generation: word(at + 7))
        at += controllerWords
        for _ in 0..<unitCount {
            let flags = word(at + 1)
            stats.units.append(Unit(guid: word(at), rxRuns: flags & 1 != 0, txRuns: flags & 2 != 0,
                                    dbcNotChecked: flags & 4 != 0, rxSpeedCode: word(at + 2),
                                    rxPackets: word(at + 3), rxCrc: word(at + 4), rxOtherStatus: word(at + 5),
                                    rxLength: word(at + 6), dbcGaps: word(at + 7), dbcBlocks: word(at + 8),
                                    rxCyclesMissed: word(at + 9), cipChanges: word(at + 10),
                                    rxLastBadStatus: word(at + 11), txPackets: word(at + 12),
                                    txStatusErrors: word(at + 13), txCyclesMissed: word(at + 14),
                                    txLastBadStatus: word(at + 15)))
            at += unitWords
        }
        for _ in 0..<asyncCount {
            stats.asyncNodes.append(AsyncNode(node: word(at), requests: word(at + 1), ackErr: word(at + 2),
                                              busy: word(at + 3), noAck: word(at + 4), timeouts: word(at + 5),
                                              other: word(at + 6)))
            at += asyncWords
        }
        for _ in 0..<portCount {
            let packed = word(at)
            stats.phyPorts.append(PhyPort(port: UInt8(packed & 0xFF),
                                          registers: (1...4).map { UInt8((packed >> UInt64($0 * 8)) & 0xFF) }))
            at += 1
        }
        return stats
    }

    private static func speedName(_ code: UInt64) -> String {
        let names = ["S100", "S200", "S400", "S800", "S1600", "S3200"]
        return code < UInt64(names.count) ? names[Int(code)] : "speed code \(code)"
    }

    /// The report, one line per entry.
    var lines: [String] {
        var out: [String] = []
        let c = controller
        out.append("controller, since the driver started: cycleLost \(c.cycleLost), cycleInconsistent \(c.cycleInconsistent), "
                   + "cycleTooLong \(c.cycleTooLong), postedWriteErr \(c.postedWriteErr), regAccessFail \(c.regAccessFail), "
                   + "unrecoverableError \(c.unrecoverableError); bus resets \(c.busResets) (generation \(c.generation))")
        if units.isEmpty {
            out.append("no unit has streamed since the driver started")
        }
        for unit in units {
            let guid = "0x" + String(unit.guid, radix: 16).leftPadded(to: 16)
            let state = unit.rxRuns || unit.txRuns ? "streaming" : "stopped (its last stream)"
            out.append("\(guid), \(state), since the stream's start:")
            let dbc = unit.dbcNotChecked ? "dbc n/a (device counts differently)"
                                         : "dbc gaps \(unit.dbcGaps) (\(unit.dbcBlocks) blocks)"
            let rxLast = unit.rxCrc + unit.rxOtherStatus != 0
                ? " (last 0x" + String(unit.rxLastBadStatus & 0xFFFF, radix: 16).leftPadded(to: 4) + ")" : ""
            out.append("  rx \(unit.rxPackets) pk \(Self.speedName(unit.rxSpeedCode)), crc \(unit.rxCrc), "
                       + "other status \(unit.rxOtherStatus)\(rxLast), length \(unit.rxLength), \(dbc), "
                       + "cycles missed \(unit.rxCyclesMissed), cip changes \(unit.cipChanges)")
            let txLast = unit.txStatusErrors != 0
                ? " (last 0x" + String(unit.txLastBadStatus & 0xFFFF, radix: 16).leftPadded(to: 4) + ")" : ""
            out.append("  tx \(unit.txPackets) pk, status err \(unit.txStatusErrors)\(txLast), "
                       + "cycles missed \(unit.txCyclesMissed)")
        }
        for node in asyncNodes {
            out.append("async to node \(node.node), since the driver started: \(node.requests) requests, "
                       + "ack err \(node.ackErr), busy \(node.busy), no ack \(node.noAck), "
                       + "timeouts \(node.timeouts), other \(node.other)")
        }
        switch phyState {
        case .skippedStreaming:
            out.append("PHY ports: not read while a stream runs (stop the audio and ask again)")
        case .unreadable:
            out.append("PHY ports: the PHY did not answer")
        case .read:
            break
        }
        for port in phyPorts {
            var words: [String] = []
            if port.connected {
                words.append("connected")
                words.append(port.child ? "child" : "parent")
                words.append(port.bias ? "bias" : "NO BIAS")
                words.append("negotiated " + Self.speedName(UInt64(port.speedCode)))
            } else {
                words.append("nothing connected")
            }
            if port.disabled { words.append("DISABLED") }
            if port.fault { words.append("FAULT") }
            let raw = port.registers.map { "0x" + String($0, radix: 16).leftPadded(to: 2) }.joined(separator: " ")
            out.append("PHY port \(port.port): \(words.joined(separator: ", ")) (registers 8-11: \(raw))")
        }
        out.append(errors == 0 ? "No error counter above zero."
                               : "Error counters above zero: \(errors) in all (busy answers and bus resets not counted).")
        return out
    }
}

private extension String {
    func leftPadded(to length: Int) -> String {
        count >= length ? self : String(repeating: "0", count: length - count) + self
    }
}

extension ASFWDriverConnector {
    /// The link metrics' totals, or nil without a connection or from a
    /// driver that has none (before b229).
    func linkStats() -> DriverLinkStats? {
        guard isConnected,
              let reply = transport.callScalarStruct(selector: Method.getLinkStats.rawValue,
                                                     scalars: [], initialCap: 4096) else {
            return nil
        }
        return DriverLinkStats.decode(reply)
    }
}
