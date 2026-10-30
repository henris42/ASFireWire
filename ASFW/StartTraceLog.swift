//
//  StartTraceLog.swift
//  ASFW
//
//  Saves the driver's numbered start steps ("[StartStep] #NN …", one line
//  per bring-up stage, logged at every log level from warn up) and its
//  "[Log]" lines from the unified log to
//  ~/Library/Logs/ASFW/start-<boot time>.log, so a start that stalled or
//  failed can be read afterwards. The dext cannot write files: the login
//  agent saves the whole boot at login and appends each later driver start.
//  The newest ten files are kept.
//

import Darwin
import Foundation

nonisolated enum StartTraceLog {
    static let directory = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Logs/ASFW")
    private static let keep = 10
    // The dext's lines reach the log as the kernel's. `log` logs its own
    // invocation, predicate and all ("log run noninteractively, args: ..."),
    // which would match the tags too.
    private static let predicate =
        #"process == "kernel" AND (eventMessage CONTAINS "[StartStep]" OR eventMessage CONTAINS "[Log]")"#
    private static let queue = DispatchQueue(label: "net.asfw.start-trace", qos: .utility)

    /// At login: this boot's start steps so far. Skipped when this boot's
    /// file exists already (the agent restarted).
    static func saveBoot(report: @escaping @Sendable (String) -> Void) {
        guard let boot = bootTime() else { return }
        let url = fileURL(boot: boot)
        guard !FileManager.default.fileExists(atPath: url.path) else { return }
        queue.async {
            guard let lines = logShow(["--last", "boot"], report: report) else { return }
            write(lines, header: "# ASFW start steps of the boot at \(stamp(boot)), saved at login\n",
                  to: url, append: false, report: report)
        }
    }

    /// A driver start seen while the agent runs: the steps since `since`,
    /// appended to this boot's file.
    static func appendStart(since: Date, report: @escaping @Sendable (String) -> Void) {
        guard let boot = bootTime() else { return }
        let url = fileURL(boot: boot)
        queue.async {
            guard let lines = logShow(["--start", logDate(since)], report: report) else { return }
            write(lines, header: "\n# Driver start seen at \(stamp(since))\n", to: url, append: true, report: report)
        }
    }

    private static func logShow(_ range: [String], report: @Sendable (String) -> Void) -> String? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/log")
        process.arguments = ["show"] + range + ["--style", "compact", "--predicate", predicate]
        let output = Pipe()
        process.standardOutput = output
        process.standardError = output
        do {
            try process.run()
        } catch {
            report("Start steps not saved: /usr/bin/log did not run (\(error.localizedDescription)).")
            return nil
        }
        let data = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        let text = String(decoding: data, as: UTF8.self)
        guard process.terminationStatus == 0 else {
            report("Start steps not saved: log show exited \(process.terminationStatus): "
                   + text.split(separator: "\n").prefix(1).joined())
            return nil
        }
        return text
    }

    private static func write(_ text: String, header: String, to url: URL, append: Bool,
                              report: @Sendable (String) -> Void) {
        let steps = text.split(separator: "\n").filter { $0.contains("[StartStep]") }.count
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let data = Data((header + text).utf8)
            if append, let handle = try? FileHandle(forWritingTo: url) {
                defer { try? handle.close() }
                try handle.seekToEnd()
                try handle.write(contentsOf: data)
            } else {
                try data.write(to: url, options: .atomic)
            }
            report("Saved \(steps) start step(s) to \(url.path).")
            prune()
        } catch {
            report("Start steps not saved to \(url.path): \(error.localizedDescription)")
        }
    }

    /// Keeps the newest files; the names sort by boot time.
    private static func prune() {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) else { return }
        let traces = names.filter { $0.hasPrefix("start-") && $0.hasSuffix(".log") }.sorted()
        for name in traces.dropLast(keep) {
            try? FileManager.default.removeItem(at: directory.appendingPathComponent(name))
        }
    }

    private static func fileURL(boot: Date) -> URL {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd-HHmmss"
        return directory.appendingPathComponent("start-\(formatter.string(from: boot)).log")
    }

    private static func stamp(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return formatter.string(from: date)
    }

    /// `log show --start` takes local time.
    private static func logDate(_ date: Date) -> String { stamp(date) }

    private static func bootTime() -> Date? {
        var boot = timeval()
        var size = MemoryLayout<timeval>.size
        guard sysctlbyname("kern.boottime", &boot, &size, nil, 0) == 0, boot.tv_sec > 0 else { return nil }
        return Date(timeIntervalSince1970: TimeInterval(boot.tv_sec) + TimeInterval(boot.tv_usec) / 1_000_000)
    }
}
