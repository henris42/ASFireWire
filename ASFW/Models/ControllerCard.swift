//
//  ControllerCard.swift
//  ASFW
//
//  One FireWire controller the driver has claimed, and the registry that
//  watches them come and go. Each controller is its own ASFWDriver service
//  (one per PCI function, all in one dext process); the app keeps a connector
//  and view models per card (CardSession) and lets the user switch between
//  them. The PCI device's registry path identifies a card across boots, which
//  is what per-card configuration is keyed by.
//

import Combine
import Foundation
import IOKit

struct ControllerCard: Identifiable, Hashable {
    /// IORegistry entry ID of the ASFWDriver service. Unique while the service
    /// lives; not stable across boots.
    let registryEntryID: UInt64
    /// IOService-plane path of the IOPCIDevice under it, e.g.
    /// `IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/RP10@1D/IOPP/PXSX@0/IOPP/pci104c,8025@0`.
    /// Stable across boots on one machine.
    let pciPath: String
    let vendorID: UInt16?
    let deviceID: UInt16?

    var id: UInt64 { registryEntryID }

    /// Last path component, e.g. `pci104c,8025@0`.
    var pciName: String {
        pciPath.split(separator: "/").last.map(String.init) ?? pciPath
    }

    var chipName: String {
        switch (vendorID, deviceID) {
        case (0x104c, 0x8025): return "TI XIO2213B"
        case (0x104c, 0x8235): return "TI XIO2213A"
        case (0x104c, _): return "Texas Instruments OHCI"
        case (0x11c1, 0x5901): return "LSI/Agere FW643"
        case (0x11c1, _): return "LSI/Agere OHCI"
        case let (vendor?, device?):
            return String(format: "FireWire controller %04x:%04x", vendor, device)
        default:
            return "FireWire controller"
        }
    }

    var displayName: String { "\(chipName) · \(pciName)" }

    /// Key for configuration that must survive a reboot.
    var persistentKey: String { pciPath }
}

/// Enumerates ASFWDriver services and publishes the cards behind them.
@MainActor
final class CardRegistry: ObservableObject {
    @Published private(set) var cards: [ControllerCard] = []

    private static let serviceName = "ASFWDriver"
    private var notificationPort: IONotificationPortRef?
    private var matchedIterator: io_iterator_t = 0
    private var terminatedIterator: io_iterator_t = 0

    init() {
        startMonitoring()
        refresh()
    }

    deinit {
        if matchedIterator != 0 { IOObjectRelease(matchedIterator) }
        if terminatedIterator != 0 { IOObjectRelease(terminatedIterator) }
        if let port = notificationPort { IONotificationPortDestroy(port) }
    }

    func refresh() {
        cards = Self.enumerate()
    }

    private func startMonitoring() {
        guard let port = IONotificationPortCreate(kIOMainPortDefault) else { return }
        notificationPort = port
        IONotificationPortSetDispatchQueue(port, .main)

        let refCon = UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
        let callback: IOServiceMatchingCallback = { refCon, iterator in
            // Drain the iterator so the notification re-arms, then re-enumerate.
            while true {
                let entry = IOIteratorNext(iterator)
                if entry == 0 { break }
                IOObjectRelease(entry)
            }
            guard let refCon else { return }
            let registry = Unmanaged<CardRegistry>.fromOpaque(refCon).takeUnretainedValue()
            DispatchQueue.main.async { registry.refresh() }
        }
        for (kind, slot) in [(kIOFirstMatchNotification, \CardRegistry.matchedIterator),
                             (kIOTerminatedNotification, \CardRegistry.terminatedIterator)] {
            var iterator: io_iterator_t = 0
            let kr = IOServiceAddMatchingNotification(port, kind, IOServiceNameMatching(Self.serviceName),
                                                      callback, refCon, &iterator)
            guard kr == KERN_SUCCESS else { continue }
            self[keyPath: slot] = iterator
            // Arm it: the initial contents are what exists now.
            while true {
                let entry = IOIteratorNext(iterator)
                if entry == 0 { break }
                IOObjectRelease(entry)
            }
        }
    }

    nonisolated static func enumerate() -> [ControllerCard] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching(serviceName),
                                           &iterator) == KERN_SUCCESS else {
            return []
        }
        defer { IOObjectRelease(iterator) }

        var cards: [ControllerCard] = []
        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 { break }
            defer { IOObjectRelease(service) }

            var entryID: UInt64 = 0
            guard IORegistryEntryGetRegistryEntryID(service, &entryID) == KERN_SUCCESS else { continue }

            var parent: io_registry_entry_t = 0
            guard IORegistryEntryGetParentEntry(service, "IOService", &parent) == KERN_SUCCESS else { continue }
            defer { IOObjectRelease(parent) }

            var pathBuffer = [CChar](repeating: 0, count: 512)
            let pathStatus = pathBuffer.withUnsafeMutableBufferPointer {
                IORegistryEntryGetPath(parent, "IOService", $0.baseAddress!)
            }
            let path = pathStatus == KERN_SUCCESS ? String(cString: pathBuffer) : "entry \(entryID)"

            cards.append(ControllerCard(registryEntryID: entryID,
                                        pciPath: path,
                                        vendorID: pciID(parent, key: "vendor-id"),
                                        deviceID: pciID(parent, key: "device-id")))
        }
        return cards.sorted { $0.pciPath < $1.pciPath }
    }

    /// PCI config IDs are published as little-endian 32-bit data.
    private nonisolated static func pciID(_ entry: io_registry_entry_t, key: String) -> UInt16? {
        guard let value = IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? Data, value.count >= 2 else {
            return nil
        }
        return UInt16(value[value.startIndex]) | (UInt16(value[value.startIndex + 1]) << 8)
    }
}
