//
//  main.swift
//  asfw-config-agent
//
//  Login agent that re-applies remembered device settings and the driver log
//  level (ConfigApplier), and saves the driver's start steps to
//  ~/Library/Logs/ASFW. With --log-level it shows or sets the log level and
//  exits (LogLevelCommand).
//  A plain command-line tool, not the app: launching the app bundle makes
//  sysextd re-check its embedded system extension and, on macOS 12 with an
//  ad-hoc signed build, re-stage and restart the running driver (b62 report
//  §3). This tool carries no extension, only the user-client entitlement.
//  install.sh puts it in /usr/local/libexec and runs it from
//  /Library/LaunchAgents/net.asfw.apply-config.plist as the logged-in user.
//

import Foundation

@main
@MainActor
enum AgentMain {
    static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        if let flag = arguments.firstIndex(of: "--log-level") {
            exit(LogLevelCommand.run(arguments.indices.contains(flag + 1) ? arguments[flag + 1] : nil))
        }
        if !arguments.isEmpty {
            print("""
                usage: asfw-config-agent                     re-apply the app's remembered device settings
                                                             and log level (run by the login agent)
                       asfw-config-agent --log-level         show the driver's log level
                       asfw-config-agent --log-level SPEC    set and save it: off, error, warn, info, debug
                                                             or trace, optionally followed by category=level
                                                             ("info irm=debug"); default forgets it
                """)
            exit(arguments.contains("--help") ? 0 : 64)
        }
        exit(ConfigApplier.runForever(store: DeviceConfigStore.readingAppContainer()))
    }
}
