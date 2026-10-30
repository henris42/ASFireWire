//
//  BootTraceDaemon.swift
//  asfw-config-agent
//
//  `asfw-config-agent --boot-trace-daemon`, run by the LaunchDaemon
//  net.asfw.boot-trace at boot (install.sh): writes the driver's lockstep
//  bring-up trace to disk as it happens, so a start that hangs the machine
//  still leaves its last completed step in a file.
//
//  With the asfw-boot-trace property on the controller (asfw-opencore-arm.sh
//  --boot-trace), the driver registers its service before it touches the
//  hardware, publishes every start step and detail step as the registry
//  properties asfw-boot-trace-lines (the newest lines) and
//  asfw-boot-trace-count (how many in all), and pauses after each one. This
//  daemon polls the registry every 10 ms, appends the new lines to
//  /var/log/asfw/boot-trace-<boot>.log and fsyncs the file each time. Without
//  the property it exits at once, so the LaunchDaemon costs nothing.
//

import Darwin
import Foundation
import IOKit

@MainActor
enum BootTraceDaemon {
    private static let directory = "/var/log/asfw"
    private static let traceProperty = "asfw-boot-trace"
    private static let linesProperty = "asfw-boot-trace-lines"
    private static let countProperty = "asfw-boot-trace-count"
    private static let driverServiceName = "ASFWDriver"
    private static let busyPollSeconds = 0.010
    private static let idlePollSeconds = 0.5
    private static let idleAfterSeconds = 30.0

    static func run() -> Int32 {
        let traced = tracedControllers()
        guard !traced.isEmpty else {
            print("asfw-config-agent --boot-trace-daemon: no FireWire controller carries \(traceProperty); exiting.")
            return 0
        }
        let path = "\(directory)/boot-trace-\(bootStamp()).log"
        guard let file = openFile(path) else {
            print("asfw-config-agent --boot-trace-daemon: cannot open \(path): \(String(cString: strerror(errno)))")
            return 1
        }
        write(file, "# ASFW boot trace, daemon started \(now()), boot \(bootSessionUUID())\n")
        for controller in traced {
            write(file, "# traced controller: \(controller)\n")
        }
        print("asfw-config-agent --boot-trace-daemon: writing \(path)")

        var written: [UInt64: UInt64] = [:]   // registry entry id -> lines written
        var lastNew = Date()
        var reportedWaiting = false
        while true {
            var sawService = false
            for entry in driverServices() {
                sawService = true
                defer { IOObjectRelease(entry) }
                var entryID: UInt64 = 0
                guard IORegistryEntryGetRegistryEntryID(entry, &entryID) == KERN_SUCCESS else { continue }
                guard let lines = driverProperty(entry, linesProperty) as? [String],
                      let total = (driverProperty(entry, countProperty) as? NSNumber)?.uint64Value else { continue }
                let have = written[entryID] ?? 0
                guard total > have else { continue }
                if written[entryID] == nil {
                    write(file, "# driver service \(String(entryID, radix: 16)) found \(now())\n")
                }
                // The property holds the newest lines; the ones past `have` are new.
                let missing = total - have
                let available = UInt64(lines.count)
                if missing > available {
                    write(file, "# \(missing - available) lines were published before this daemon read them\n")
                }
                let fresh = lines.suffix(Int(min(missing, available)))
                for line in fresh {
                    write(file, "\(now()) \(line)\n")
                }
                written[entryID] = total
                lastNew = Date()
            }
            if !sawService && !reportedWaiting && Date().timeIntervalSince(lastNew) > 60 {
                write(file, "# no ASFWDriver service registered after 60 s (not armed, or its Start has not run)\n")
                reportedWaiting = true
            }
            let idle = Date().timeIntervalSince(lastNew) > idleAfterSeconds
            Thread.sleep(forTimeInterval: idle ? idlePollSeconds : busyPollSeconds)
        }
    }

    /// The PCI paths of the FireWire controllers that carry the trace property.
    private static func tracedControllers() -> [String] {
        guard let matching = IOServiceMatching("IOPCIDevice") else { return [] }
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var paths: [String] = []
        while true {
            let entry = IOIteratorNext(iterator)
            if entry == 0 { break }
            defer { IOObjectRelease(entry) }
            guard property(entry, traceProperty) != nil else { continue }
            var buffer = [CChar](repeating: 0, count: 512)
            let status = buffer.withUnsafeMutableBufferPointer { IORegistryEntryGetPath(entry, "IOService", $0.baseAddress!) }
            paths.append(status == KERN_SUCCESS ? String(cString: buffer) : "(unnamed)")
        }
        return paths
    }

    private static func driverServices() -> [io_registry_entry_t] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching(driverServiceName),
                                           &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var entries: [io_registry_entry_t] = []
        while true {
            let entry = IOIteratorNext(iterator)
            if entry == 0 { break }
            entries.append(entry)
        }
        return entries
    }

    private static func property(_ entry: io_registry_entry_t, _ key: String) -> Any? {
        IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
    }

    /// A property the dext set on itself: the kernel keeps a dext's
    /// SetProperties under IOUserServiceProperties (xnu IOUserServer.cpp).
    private static func driverProperty(_ entry: io_registry_entry_t, _ key: String) -> Any? {
        if let user = property(entry, "IOUserServiceProperties") as? [String: Any], let value = user[key] {
            return value
        }
        return property(entry, key)
    }

    private static func openFile(_ path: String) -> Int32? {
        mkdir(directory, 0o755)
        let fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0o644)
        return fd >= 0 ? fd : nil
    }

    /// Appends and makes sure it is on disk before returning: the next driver
    /// step waits for nobody, so the write has to land during the pause.
    private static func write(_ fd: Int32, _ text: String) {
        var bytes = Array(text.utf8)
        bytes.withUnsafeMutableBytes { raw in
            var offset = 0
            while offset < raw.count {
                let n = Darwin.write(fd, raw.baseAddress! + offset, raw.count - offset)
                if n <= 0 { break }
                offset += Int(n)
            }
        }
        _ = fcntl(fd, F_FULLFSYNC)
    }

    private static func bootSessionUUID() -> String {
        var size = 0
        guard sysctlbyname("kern.bootsessionuuid", nil, &size, nil, 0) == 0, size > 0 else { return "unknown" }
        var buffer = [CChar](repeating: 0, count: size)
        guard sysctlbyname("kern.bootsessionuuid", &buffer, &size, nil, 0) == 0 else { return "unknown" }
        return String(cString: buffer)
    }

    private static func bootStamp() -> String {
        var tv = timeval()
        var size = MemoryLayout<timeval>.size
        var mib: [Int32] = [CTL_KERN, KERN_BOOTTIME]
        let date = sysctl(&mib, 2, &tv, &size, nil, 0) == 0
            ? Date(timeIntervalSince1970: TimeInterval(tv.tv_sec)) : Date()
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return formatter.string(from: date)
    }

    private static func now() -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter.string(from: Date())
    }
}
