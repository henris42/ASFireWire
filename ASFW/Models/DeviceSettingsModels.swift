//
//  DeviceSettingsModels.swift
//  ASFW
//
//  Host-settable device configuration: the clock source and what the optical
//  ports carry. Mirrors ASFWDriver/Audio/Protocols/DeviceSettings.hpp and the
//  user client's DeviceSettingsWire.hpp.
//

import Foundation

enum DeviceSetting: UInt32, CaseIterable {
    case clockSource = 1
    case opticalInputMode = 2
    case opticalOutputMode = 3

    var title: String {
        switch self {
        case .clockSource: return "Clock Source"
        case .opticalInputMode: return "Optical In"
        case .opticalOutputMode: return "Optical Out"
        }
    }
}

enum DeviceClockSource: UInt32, CaseIterable, Identifiable {
    case internalClock = 0
    case adatOptical = 1
    case spdif = 2
    case busSph = 3
    case wordClockBnc = 4
    case adatDsub = 5
    case aesEbuXlr = 7

    var id: UInt32 { rawValue }

    var title: String {
        switch self {
        case .internalClock: return "Internal"
        case .adatOptical: return "ADAT optical"
        case .spdif: return "S/PDIF"
        case .busSph: return "FireWire bus (SPH)"
        case .wordClockBnc: return "Word clock (BNC)"
        case .adatDsub: return "ADAT D-sub"
        case .aesEbuXlr: return "AES/EBU (XLR)"
        }
    }
}

enum DeviceOpticalMode: UInt32, CaseIterable, Identifiable {
    case off = 0
    case adat = 1
    case spdif = 2

    var id: UInt32 { rawValue }

    var title: String {
        switch self {
        case .off: return "Off"
        case .adat: return "ADAT"
        case .spdif: return "TOSLINK (S/PDIF)"
        }
    }
}

struct DeviceSettingsSnapshot: Equatable {
    var supportedSettings: UInt32 = 0
    var clockSource: UInt32 = 0
    var clockSourceOptions: UInt32 = 0
    var opticalInputMode: UInt32 = 0
    var opticalOutputMode: UInt32 = 0
    var opticalModeOptions: UInt32 = 0
    var clockRaw: UInt32 = 0
    var opticalRaw: UInt32 = 0

    func supports(_ setting: DeviceSetting) -> Bool {
        supportedSettings & (1 << setting.rawValue) != 0
    }

    var clockSourceChoices: [DeviceClockSource] {
        DeviceClockSource.allCases.filter { clockSourceOptions & (1 << $0.rawValue) != 0 }
    }

    var opticalModeChoices: [DeviceOpticalMode] {
        DeviceOpticalMode.allCases.filter { opticalModeOptions & (1 << $0.rawValue) != 0 }
    }

    /// DeviceSettingsResultWire, little-endian, 48 bytes.
    static let wireSize = 48

    init() {}

    init?(wire data: Data) {
        guard data.count >= Self.wireSize else { return nil }
        func u32(_ offset: Int) -> UInt32 {
            data.subdata(in: (data.startIndex + offset)..<(data.startIndex + offset + 4))
                .withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
        }
        guard u32(0) == 1 else { return nil }   // version
        supportedSettings = u32(16)
        clockSource = u32(20)
        clockSourceOptions = u32(24)
        opticalInputMode = u32(28)
        opticalOutputMode = u32(32)
        opticalModeOptions = u32(36)
        clockRaw = u32(40)
        opticalRaw = u32(44)
    }

    /// The transaction's own status is bytes 4..<8 of the wire structure.
    static func wireStatus(_ data: Data) -> kern_return_t? {
        guard data.count >= 8 else { return nil }
        let value = data.subdata(in: (data.startIndex + 4)..<(data.startIndex + 8))
            .withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
        return kern_return_t(bitPattern: value)
    }
}

/// Outcome of a settings request as the app polls for it.
enum DeviceSettingsOutcome {
    case pending
    case ready(DeviceSettingsSnapshot)
    case failed(kern_return_t)
}


/// Knobs for finding out why the 828mkII resets the bus on the first data packet
/// (driver: ASFWDriver/Audio/Protocols/MOTU/MotuExperiments.hpp). Not persisted.
struct MotuExperiment: Equatable {
    static let forceFetchEnableFlag: UInt32 = 1 << 0
    static let captureOnlyFlag: UInt32 = 1 << 1
    static let holdAbsolutePhaseFlag: UInt32 = 1 << 2
    static let transmitOnlyFlag: UInt32 = 1 << 3
    static let skipCyclePrefillFlag: UInt32 = 1 << 4
    static let startEventDbcFlag: UInt32 = 1 << 5

    var forceFetchEnable = false
    var captureOnly = false
    /// Hold the absolute SPH phase at the 828mk3 setpoint (the pre-b98 behaviour on the mk2).
    var holdAbsolutePhase = false
    /// Start the transmit context only (no receive): the counterpart of capture only.
    var transmitOnly = false
    /// Prime the ring with skip cycles (nothing on the wire) like Linux, not 1512 empty CIP packets.
    var skipCyclePrefill = false
    /// Count DBC as the 828mkII itself does (data: first block, empty: next packet).
    var startEventDbc = false
    /// Packets (125 µs each) to keep sending as NO-DATA after the prefill.
    var extraNoDataPackets: UInt32 = 0
    /// SPH presentation lead in bus cycles for the next start; 0 = the model's default.
    var presentationLeadCycles: UInt32 = 0

    var flags: UInt32 {
        (forceFetchEnable ? Self.forceFetchEnableFlag : 0)
            | (captureOnly ? Self.captureOnlyFlag : 0)
            | (holdAbsolutePhase ? Self.holdAbsolutePhaseFlag : 0)
            | (transmitOnly ? Self.transmitOnlyFlag : 0)
            | (skipCyclePrefill ? Self.skipCyclePrefillFlag : 0)
            | (startEventDbc ? Self.startEventDbcFlag : 0)
    }
    var presentationLeadTicks: UInt32 { presentationLeadCycles * 3072 }
    var isDefault: Bool { self == MotuExperiment() }

    static let leadChoices: [(title: String, cycles: UInt32)] = [
        ("model default", 0), ("2 cycles", 2), ("3 cycles", 3), ("4 cycles", 4), ("5 cycles", 5), ("6 cycles", 6),
    ]

    static let dataDelayChoices: [(title: String, packets: UInt32)] = [
        ("none", 0), ("250 ms", 2000), ("1 s", 8000), ("4 s", 32000),
    ]
}
