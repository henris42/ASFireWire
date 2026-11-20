//
//  BusStallCommand.swift
//  asfw-config-agent
//
//  `asfw-config-agent --bus-stall [LIMIT]`: shows the CPU idle limit the
//  driver holds while a device runs IO, or sets it on the running driver and
//  saves it, so the login agent sets it again after each driver start. LIMIT
//  is off, or 5, 10, 20, 25, 30 or 40 (microseconds: the longest wake-up from
//  an idle state the system may choose while audio runs); "default" forgets
//  the saved limit and goes back to 5. A new limit holds from each device's
//  next IO start.
//

import Foundation

@MainActor
enum BusStallCommand {
    static func run(_ spec: String?) -> Int32 {
        let connectors = CardRegistry.enumerate().map { card -> (ControllerCard, ASFWDriverConnector) in
            let connector = ASFWDriverConnector(registryEntryID: card.registryEntryID, pollsStatus: false)
            _ = connector.connect(forceAttempt: true)
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
            let saved = DeviceConfigStore.readingAppContainer().busStall
            print("Saved CPU idle limit: \(saved.map(describe) ?? "none (the driver's default, 5 us)")")
            if running.isEmpty {
                print("No controller is running the driver.")
                for (card, connector) in connectors {
                    print("\(card.displayName): \(connector.lastError ?? "no connection within 3 s")")
                }
            }
            for (card, connector) in running {
                if let state = connector.busStall() {
                    print("\(card.displayName): \(describe(state.spec)); "
                          + (state.holders == 0 ? "no device runs IO with a limit held"
                                                : "held now by \(state.holders) device(s) running IO"))
                } else {
                    print("\(card.displayName): CPU idle limit not readable (a driver before b226?)")
                }
            }
            return 0
        }

        let forget = spec == "default"
        let wanted = forget ? ASFWDriverConnector.defaultBusStall : spec.lowercased()
        guard let nanos = ASFWDriverConnector.busStallNanos(wanted) else {
            print("asfw-config-agent: '\(spec)' is not a CPU idle limit. Use off, 5, 10, 20, 25, 30 or 40 "
                  + "(microseconds), or default.")
            return 64
        }
        var applied: DriverBusStall?
        for (card, connector) in running {
            guard let state = connector.setBusStall(nanos: nanos) else {
                print("\(card.displayName): the driver did not take '\(wanted)'; nothing saved.")
                return 65
            }
            applied = state
        }
        DeviceConfigStore.saveBusStallForApp(forget ? nil : wanted)
        if let applied {
            print("CPU idle limit \(describe(applied.spec))"
                  + (forget ? "; the saved limit is forgotten." : ", saved: the login agent sets it again after each driver start."))
            if applied.holders > 0 {
                print("\(applied.holders) device(s) run IO now and keep the limit they started with; "
                      + "stop and start the audio client for the new one.")
            }
        } else {
            print(forget ? "The saved CPU idle limit is forgotten; the driver starts at 5 us."
                         : "No controller is running the driver; \(describe(wanted)) is saved for its next start.")
        }
        return 0
    }

    private static func describe(_ spec: String) -> String {
        spec == "off" ? "off" : "\(spec) us"
    }
}
