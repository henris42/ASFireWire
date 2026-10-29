//
//  LogLevelCommand.swift
//  asfw-config-agent
//
//  `asfw-config-agent --log-level [SPEC]`: shows the driver's log level, or
//  sets it at once on the running driver and saves it, so the login agent
//  sets it again after each driver start. SPEC is a level (off, error, warn,
//  info, debug, trace), optionally followed by category=level overrides
//  ("info irm=debug isoch=trace"); "default" forgets the saved level and
//  goes back to warn.
//

import Foundation

@MainActor
enum LogLevelCommand {
    static func run(_ spec: String?) -> Int32 {
        let connectors = CardRegistry.enumerate().map { card -> (ControllerCard, ASFWDriverConnector) in
            let connector = ASFWDriverConnector(registryEntryID: card.registryEntryID)
            connector.connect(forceAttempt: true)
            return (card, connector)
        }
        // The connection opens on the connector's queue and reports on the main one.
        let deadline = Date().addingTimeInterval(3)
        while connectors.contains(where: { !$0.1.isConnected }) && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        defer { connectors.forEach { $0.1.disconnect() } }
        let running = connectors.filter { $0.1.isConnected }

        guard let spec else {
            let saved = DeviceConfigStore.readingAppContainer().logLevel
            print("Saved log level: \(saved ?? "none (the driver's default, warn)")")
            if running.isEmpty {
                print("No controller is running the driver.")
            }
            for (card, connector) in running {
                if let level = connector.logLevel() {
                    print("\(card.displayName): \(level.spec), set by \(level.source.title)")
                } else {
                    print("\(card.displayName): log level not readable")
                }
            }
            return 0
        }

        let forget = spec == "default"
        let wanted = forget ? ASFWDriverConnector.defaultLogLevel : spec
        guard ASFWDriverConnector.isValidLogLevelSpec(wanted) else {
            print("asfw-config-agent: '\(spec)' is not a log level. Use off, error, warn, info, debug or trace, "
                  + "optionally followed by category=level (e.g. \"info irm=debug\"), or default.")
            return 64
        }
        var applied: DriverLogLevel?
        for (card, connector) in running {
            guard let level = connector.setLogLevel(wanted, source: .commandLine) else {
                print("\(card.displayName): the driver refused '\(wanted)' (an unknown category?); nothing saved.")
                return 65
            }
            applied = level
        }
        DeviceConfigStore.saveLogLevelForApp(forget ? nil : (applied?.spec ?? wanted))
        if let applied {
            print("Log level \(applied.spec)" + (forget ? "; the saved level is forgotten." : ", saved: the login agent sets it again after each driver start."))
        } else {
            print(forget ? "The saved log level is forgotten; the driver starts at warn."
                         : "No controller is running the driver; \(wanted) is saved for its next start.")
        }
        return 0
    }
}
