//
//  ASFWApp.swift
//  ASFW
//
//  Created by Alexander Shabelnikov on 21.09.2025.
//

import SwiftUI

#if ASFW_LITE
// macOS 12/13 build: no Metal analyzer, meters, DV capture or MCP (see
// tools/derive-spec.py). Doubles as the driver installer's headless tool for
// install.sh (`ASFW --activate | --deactivate | --status`), like ASFWLoader.
@main
enum ASFWMain {
    static func main() {
        if let command = LoaderCommandLine.command(from: CommandLine.arguments) {
            exit(LoaderCommandLine.run(command))
        }
        ASFWApp.main()
    }
}

struct ASFWApp: App {
    var body: some Scene {
        WindowGroup {
            ModernContentView()
        }
    }
}
#else
@main
struct ASFWApp: App {
    var body: some Scene {
        WindowGroup {
            ModernContentView()
        }
        .defaultSize(width: 1440, height: 900)
    }
}
#endif
