//
//  main.swift
//  asfw-config-agent
//
//  Login agent that re-applies remembered device settings (ConfigApplier).
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
        if CommandLine.arguments.dropFirst().contains("--help") {
            print("asfw-config-agent: re-applies the ASFW app's remembered device settings; no arguments.")
            exit(64)
        }
        exit(ConfigApplier.runForever(store: DeviceConfigStore.readingAppContainer()))
    }
}
