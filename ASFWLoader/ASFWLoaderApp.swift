//
//  ASFWLoaderApp.swift
//  ASFW (loader variant)
//
//  Stand-in for ASFW.app on macOS releases older than the full app supports
//  (it needs macOS 14). Same bundle identifier, entitlements and embedded dext,
//  so system-extension approval is unchanged. It only activates, deactivates and
//  reports on the driver, from its window or headless for install.sh
//  (LoaderCommandLine.swift); diagnostics need the full app.
//  Built by tools/build-for-macos.sh for ventura and older.
//

import Combine
import SwiftUI

@main
enum ASFWLoaderMain {
    static func main() {
        if let command = LoaderCommandLine.command(from: CommandLine.arguments) {
            exit(LoaderCommandLine.run(command))
        }
        ASFWLoaderApp.main()
    }
}

struct ASFWLoaderApp: App {
    var body: some Scene {
        WindowGroup {
            LoaderView()
        }
    }
}

final class LoaderModel: ObservableObject {
    @Published private(set) var snapshot = DriverExtensionRegistryInspector.snapshot()
    @Published private(set) var isBusy = false
    @Published private(set) var log: [String] = []

    let buildNumber = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?"

    func refresh() {
        snapshot = DriverExtensionRegistryInspector.snapshot()
    }

    func activate() {
        append("Requesting activation of build \(buildNumber)…")
        run { DriverInstallManager.shared.activate(progress: $0, completion: $1) }
    }

    func deactivate() {
        append("Requesting deactivation…")
        run { DriverInstallManager.shared.deactivate(progress: $0, completion: $1) }
    }

    private func run(_ operation: (@escaping DriverInstallManager.ProgressHandler,
                                   @escaping DriverInstallManager.CompletionHandler) -> Void) {
        isBusy = true
        operation({ [weak self] message in
            self?.append(message)
        }, { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let message):
                self.append(message)
            case .failure(let error):
                let suggestion = (error as? LocalizedError)?.recoverySuggestion.map { " \($0)" } ?? ""
                self.append("Failed: \(error.localizedDescription)\(suggestion)")
            }
            self.isBusy = false
            self.refresh()
        })
    }

    private func append(_ line: String) {
        log.append(line)
    }
}

struct LoaderView: View {
    @StateObject private var model = LoaderModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("ASFireWire Driver")
                .font(.title2)
            Text("Build \(model.buildNumber) · driver installer only")
                .foregroundColor(.secondary)

            GroupBox("Status") {
                VStack(alignment: .leading, spacing: 4) {
                    statusRow("FireWire controller present", model.snapshot.controllerPresent)
                    statusRow("ASFWDriver attached", model.snapshot.driverAttached)
                    statusRow("DriverKit server running", model.snapshot.serverPresent)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            HStack {
                Button("Activate", action: model.activate)
                Button("Deactivate", action: model.deactivate)
                Button("Refresh", action: model.refresh)
                if model.isBusy {
                    ProgressView()
                        .scaleEffect(0.6)
                }
            }
            .disabled(model.isBusy)

            ScrollView {
                Text(model.log.joined(separator: "\n"))
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(minHeight: 120)

            Text("On first activation, allow the extension in System Preferences › Security & Privacy › General.")
                .font(.caption)
                .foregroundColor(.secondary)
        }
        .padding()
        .frame(minWidth: 520, minHeight: 380)
    }

    private func statusRow(_ label: String, _ ok: Bool) -> some View {
        HStack {
            Image(systemName: ok ? "checkmark.circle.fill" : "xmark.circle")
                .foregroundColor(ok ? .green : .secondary)
            Text(label)
        }
    }
}
