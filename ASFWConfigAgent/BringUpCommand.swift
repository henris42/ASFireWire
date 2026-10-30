//
//  BringUpCommand.swift
//  asfw-config-agent
//
//  `asfw-config-agent --bring-up` and `--bring-down`: ask every running ASFW
//  controller driver to bring its runtime up (a controller claimed with the
//  asfw-start = manual boot property sits idle until then), or to quiesce it
//  again as for sleep. The steps show as [StartStep] lines in the unified log
//  and, with asfw-boot-trace set, in the boot trace file.
//

import Foundation

@MainActor
enum BringUpCommand {
    static func run(up: Bool) -> Int32 {
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
            print("No controller is running the driver (is it armed, and did its Start return?).")
            for (card, connector) in connectors {
                print("\(card.displayName): \(connector.lastError ?? "no connection within 3 s")")
            }
            return 65
        }
        var failed = false
        for (card, connector) in running {
            let accepted = up ? connector.requestBringUp() : connector.requestBringDown()
            if accepted {
                print("\(card.displayName): \(up ? "bring-up" : "bring-down") requested; follow the "
                      + "[StartStep] lines (log stream --predicate 'eventMessage CONTAINS \"[StartStep]\"')"
                      + (up ? " or the boot trace file in /var/log/asfw." : "."))
            } else {
                print("\(card.displayName): the driver refused the request (already running, or no provider).")
                failed = true
            }
        }
        return failed ? 66 : 0
    }
}
