//
//  main.swift
//  asfw-config-agent
//
//  Login agent that re-applies remembered device settings and the driver log
//  level (ConfigApplier), and saves the driver's start steps to
//  ~/Library/Logs/ASFW. With --log-level it shows or sets the log level and
//  exits (LogLevelCommand); with --bus-stall, the CPU idle limit held while
//  audio runs (BusStallCommand); with --link-stats, what the driver has counted
//  of link, packet and bus errors (LinkStatsCommand).
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
        if let flag = arguments.firstIndex(of: "--bus-stall") {
            exit(BusStallCommand.run(arguments.indices.contains(flag + 1) ? arguments[flag + 1] : nil))
        }
        if arguments.contains("--link-stats") {
            exit(LinkStatsCommand.run())
        }
        if arguments.contains("--bring-up") {
            exit(BringUpCommand.run(up: true))
        }
        if arguments.contains("--bring-down") {
            exit(BringUpCommand.run(up: false))
        }
        if arguments.contains("--boot-trace-daemon") {
            exit(BootTraceDaemon.run())
        }
        if !arguments.isEmpty {
            print("""
                usage: asfw-config-agent                     re-apply the app's remembered device settings
                                                             and log level (run by the login agent)
                       asfw-config-agent --log-level         show the driver's log level
                       asfw-config-agent --log-level SPEC    set and save it: off, error, warn, info, debug
                                                             or trace, optionally followed by category=level
                                                             ("info irm=debug"); default forgets it
                       asfw-config-agent --bus-stall         show the CPU idle limit held while audio runs
                       asfw-config-agent --bus-stall LIMIT   set and save it: off, or 5, 10, 20, 25, 30 or 40
                                                             (us; the longest idle-state wake-up allowed
                                                             while a device runs IO); default forgets it
                       asfw-config-agent --link-stats        show what the driver has counted of link, packet
                                                             and bus errors, and the PHY's port status
                       asfw-config-agent --bring-up          run the controller bring-up now (a controller
                                                             armed with asfw-start = manual waits for this)
                       asfw-config-agent --bring-down        quiesce the runtime again, as for sleep
                       asfw-config-agent --boot-trace-daemon write the lockstep boot trace to /var/log/asfw
                                                             (the LaunchDaemon net.asfw.boot-trace; exits
                                                             unless a controller carries asfw-boot-trace)
                """)
            exit(arguments.contains("--help") ? 0 : 64)
        }
        exit(ConfigApplier.runForever(store: DeviceConfigStore.readingAppContainer()))
    }
}
