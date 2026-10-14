//
//  LoaderCommandLine.swift
//  ASFW (loader variant)
//
//  Headless driver activation for install.sh, so installing or updating needs
//  no clicks in the app:
//
//    ASFW.app/Contents/MacOS/ASFW --activate | --deactivate | --status
//
//  Run it from the logged-in user's session (Terminal, or over ssh through
//  `launchctl asuser`): system-extension requests need a user session. Each
//  request has a deadline, so this never waits forever.
//
//  Exit status: 0 done, 1 failed, 2 still waiting (approval or reboot) at the
//  deadline, 64 bad usage.
//

import Foundation

enum LoaderCommandLine {
    enum Command: String {
        case activate = "--activate"
        case deactivate = "--deactivate"
        case status = "--status"
    }

    /// The requested command, or nil to start the window UI.
    static func command(from arguments: [String]) -> Command? {
        arguments.dropFirst().lazy.compactMap(Command.init(rawValue:)).first
    }

    /// Long enough to click Allow in Security & Privacy after the prompt.
    static let requestTimeout: TimeInterval = 120

    static func run(_ command: Command) -> Int32 {
        switch command {
        case .status:
            printStatus()
            return 0
        case .activate:
            return runRequest("activation") { progress, completion in
                DriverInstallManager.shared.activate(progress: progress, completion: completion)
            }
        case .deactivate:
            return runRequest("deactivation") { progress, completion in
                DriverInstallManager.shared.deactivate(progress: progress, completion: completion)
            }
        }
    }

    private static func runRequest(
        _ name: String,
        _ submit: (@escaping DriverInstallManager.ProgressHandler,
                   @escaping DriverInstallManager.CompletionHandler) -> Void
    ) -> Int32 {
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?"
        say("Requesting \(name) of build \(build)…")

        var status: Int32?
        submit({ say($0) }, { result in
            switch result {
            case .success(let message):
                say(message)
                status = 0
            case .failure(let error):
                // Activating the build that is already active is not a failure:
                // a rerun after a reboot or an approval lands here.
                if case let DriverInstallManager.OperationError.replacementVersionNotNewer(existing, replacement)? =
                    error as? DriverInstallManager.OperationError, existing == replacement {
                    say("Build \(existing) is already the active driver.")
                    status = 0
                    return
                }
                let suggestion = (error as? LocalizedError)?.recoverySuggestion.map { " \($0)" } ?? ""
                say("Failed: \(error.localizedDescription)\(suggestion)")
                status = 1
            }
        })

        let deadline = Date().addingTimeInterval(requestTimeout)
        while status == nil && Date() < deadline {
            _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.25))
        }
        printStatus()
        if let status {
            return status
        }
        say("No answer after \(Int(requestTimeout)) s: the request is still pending (approval or reboot).")
        return 2
    }

    private static func printStatus() {
        let snapshot = DriverExtensionRegistryInspector.snapshot()
        say("controller present: \(snapshot.controllerPresent ? "yes" : "no"), "
            + "driver attached: \(snapshot.driverAttached ? "yes" : "no"), "
            + "DriverKit server: \(snapshot.serverPresent ? "running" : "not running")")
    }

    private static func say(_ line: String) {
        FileHandle.standardOutput.write(Data("[ASFW] \(line)\n".utf8))
    }
}
