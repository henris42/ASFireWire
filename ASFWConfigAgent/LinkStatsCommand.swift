//
//  LinkStatsCommand.swift
//  asfw-config-agent
//
//  `asfw-config-agent --link-stats`: prints, for every running controller,
//  what the driver has counted of link, packet and bus errors: the
//  controller's own events, each streaming unit's received and sent packets
//  since its stream started, the asynchronous requests per node, and the
//  local PHY's port status (read only while no stream runs). The same
//  counters feed the driver's [Link] log lines. Reads only; changes nothing.
//

import Foundation

@MainActor
enum LinkStatsCommand {
    static func run() -> Int32 {
        let connectors = CardRegistry.enumerate().map { card -> (ControllerCard, ASFWDriverConnector) in
            let connector = ASFWDriverConnector(registryEntryID: card.registryEntryID, pollsStatus: false)
            _ = connector.connect(forceAttempt: true)
            return (card, connector)
        }
        let deadline = Date().addingTimeInterval(3)
        while connectors.contains(where: { !$0.1.isConnected }) && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        defer { connectors.forEach { $0.1.disconnect() } }
        let running = connectors.filter { $0.1.isConnected }
        if running.isEmpty {
            print("No controller is running the driver.")
            for (card, connector) in connectors {
                print("\(card.displayName): \(connector.lastError ?? "no connection within 3 s")")
            }
            return 65
        }
        var failed = false
        for (card, connector) in running {
            print("\(card.displayName):")
            if let stats = connector.linkStats() {
                stats.lines.forEach { print("  \($0)") }
            } else {
                print("  link statistics not readable (a driver before b229, or its runtime is not up)")
                failed = true
            }
        }
        return failed ? 66 : 0
    }
}
