//
//  OverviewView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI

struct OverviewView: View {
    @ObservedObject var viewModel: DriverViewModel
    /// The controllers the driver claims and their app sessions.
    @ObservedObject var workspace: CardWorkspace
    @Binding var requireNewerBuild: Bool
    
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                // Header
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Image(systemName: "cpu.fill")
                            .font(.system(size: 40))
                            .foregroundStyle(.blue.gradient)
                        
                        VStack(alignment: .leading, spacing: 4) {
                            Text("ASFW DriverKit Extension")
                                .font(.title.bold())
                            Text("FireWire OHCI Host Controller Driver")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                        }
                    }
                    
                    Divider()
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                
                // Driver Management Card
                VStack(alignment: .leading, spacing: 12) {
                    Label("Driver Management", systemImage: "circle.hexagonpath")
                        .font(.headline)
                    
                    HStack(spacing: 10) {
                        statusIndicator
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Current status")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                            Text(viewModel.displayedStatus)
                                .font(.system(.body, design: .monospaced))
                        }
                        Spacer()
                    }
                    .padding()
                    .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))

                    HStack(spacing: 12) {
                        Button {
                            viewModel.installDriver(requireNewerBuild: requireNewerBuild)
                        } label: {
                            Label("Install", systemImage: "arrow.down.circle.fill")
                        }
                        .labelStyle(.titleAndIcon)
                        .buttonStyle(.borderedProminent)
                        .disabled(viewModel.isBusy)

                        Button(role: .destructive) {
                            viewModel.uninstallDriver()
                        } label: {
                            Label("Delete", systemImage: "trash.fill")
                        }
                        .labelStyle(.titleAndIcon)
                        .buttonStyle(.bordered)
                        .disabled(viewModel.isBusy)
                    }

                    #if DEBUG
                    Divider()

                    Toggle(isOn: $requireNewerBuild) {
                        VStack(alignment: .leading, spacing: 3) {
                            Text("Require a newer build")
                            Text("Only replace an installed driver when the incoming build number is higher.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .toggleStyle(.switch)
                    .disabled(viewModel.isBusy)
                    .help("When enabled, ASFW rejects an equal or older build during installation.")
                    #endif
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                
                // Controllers Card: what the driver actually claims right now.
                VStack(alignment: .leading, spacing: 12) {
                    Label("Controllers", systemImage: "pcicard")
                        .font(.headline)

                    if workspace.cards.isEmpty {
                        Text("No FireWire controller is driven by ASFW. The driver must be activated and the controller armed (asfw-armed) for it to match.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(workspace.cards) { card in
                            let session = workspace.sessions[card.id]
                            let connected = session?.debugVM.isConnected ?? false
                            HStack(alignment: .top, spacing: 10) {
                                Image(systemName: connected ? "checkmark.circle.fill" : "xmark.circle")
                                    .foregroundStyle(connected ? Color.green : Color.secondary)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(card.displayName)
                                        .font(.system(.body, design: .monospaced))
                                    Text(card.pciPath)
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                    if !connected, let error = session?.connector.lastError {
                                        Text(error)
                                            .font(.caption)
                                            .foregroundStyle(.orange)
                                    }
                                }
                            }
                        }
                    }
                    InfoRow(label: "Protocol", value: "IEEE 1394 OHCI 1.1")
                    InfoRow(label: "Bundle ID", value: "net.asfw.driver")
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))

                // Connection Log Card: the app side of each driver connection
                // (open, close reasons, failed calls). Also in os_log `net.asfw.app`.
                VStack(alignment: .leading, spacing: 12) {
                    Label("Connection Log", systemImage: "text.alignleft")
                        .font(.headline)
                    if viewModel.logMessages.isEmpty {
                        Text("Nothing yet.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    } else {
                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(viewModel.logMessages.suffix(40)) { entry in
                                Text("\(entry.formattedTime) [\(entry.source.rawValue)] \(entry.message)")
                                    .font(.system(.caption, design: .monospaced))
                                    .foregroundStyle(entry.level.color)
                                    .textSelection(.enabled)
                            }
                        }
                    }
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                
                // Build Metadata Card
                if let version = viewModel.driverVersion {
                    VStack(alignment: .leading, spacing: 12) {
                        Label("Build Metadata", systemImage: "hammer.fill")
                            .font(.headline)
                        
                        InfoRow(label: "Version", value: version.semanticVersion)
                        InfoRow(label: "Commit", value: "\(version.gitCommitShort) (\(version.gitBranch))")
                        InfoRow(label: "Built", value: version.buildTimestamp)
                        InfoRow(label: "Host", value: version.buildHost)
                        if version.gitDirty {
                            Text("⚠️ Built with uncommitted changes")
                                .font(.caption)
                                .foregroundStyle(.orange)
                        }
                    }
                    .padding()
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                }
                
                // Help Text
                Text("After the first installation, macOS may ask you to approve ASFW in System Settings > General > Login Items & Extensions > Driver Extensions.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding()
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
            }
            .padding()
        }
        .navigationTitle("Overview")
    }
    
    @ViewBuilder
    private var statusIndicator: some View {
        let isActive = viewModel.isDriverConnected
        let isError = !isActive && viewModel.activationStatus.contains("Error")
        
        Circle()
            .fill(viewModel.isBusy ? Color.orange : (isError ? Color.red : (isActive ? Color.green : Color.gray)))
            .frame(width: 12, height: 12)
            .overlay(
                Circle()
                    .stroke(viewModel.isBusy ? Color.orange : (isError ? Color.red : (isActive ? Color.green : Color.gray)), lineWidth: 2)
                    .scaleEffect(viewModel.isBusy ? 1.3 : 1.0)
                    .opacity(viewModel.isBusy ? 0 : 1)
                    .animation(viewModel.isBusy ? .easeInOut(duration: 1.0).repeatForever(autoreverses: true) : .default, value: viewModel.isBusy)
            )
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("Driver status indicator")
            .accessibilityValue(viewModel.displayedStatus)
    }
}
