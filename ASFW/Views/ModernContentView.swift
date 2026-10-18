//
//  ModernContentView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI
import Foundation

struct ModernContentView: View {
    @StateObject private var driverVM = DriverViewModel()
    /// One session (connector + view models) per FireWire controller the driver
    /// claims; the sidebar picks which one the window shows.
    @StateObject private var workspace = CardWorkspace()
    @State private var publishedAudioDevices: [AudioWrapperDevice] = []
    @State private var selectedSection: SidebarSection? = .overview
    @AppStorage(AdvancedToolsSettings.storageKey)
    private var showAdvancedTools = AdvancedToolsSettings.defaultEnabled
    @AppStorage(DriverInstallSettings.requireNewerBuildKey)
    private var requireNewerBuild = DriverInstallSettings.defaultRequireNewerBuild

    enum SidebarSection: String, CaseIterable, Identifiable {
        case overview = "Overview"
        case busInspector = "Bus Inspector"
        case deviceSettings = "Device Settings"
        case avcUnits = "AV/C Units"
        case avcCommands = "AV/C Commands"
        case controller = "Controller Status"
        case async = "Async Commands"
#if !ASFW_LITE
        case audioTelemetry = "Audio Telemetry"
#endif
        case audioGeometry = "Audio Geometry"
#if !ASFW_LITE
        case dvCapture = "DV Capture"
#endif
        case busReset = "Bus Reset History"
        case logs = "System Logs"
        case loggingSettings = "Logging Settings"
#if !ASFW_LITE
        case mcpSettings = "MCP Control"
#endif
        case audio = "Core Audio"
#if !ASFW_LITE
        case audioAnalyzer = "Audio Analyzer"
#endif
        case duet = "Duet"
        case diagnostics = "Driver Report"
        case diceReport = "DICE Report"
        case avcReport = "AV/C Report"

        var id: String { rawValue }

        var systemImage: String {
            switch self {
            case .overview: return "info.circle"
            case .busInspector: return "network"
            case .deviceSettings: return "dial.min"
            case .avcUnits: return "music.note"
            case .avcCommands: return "command"
            case .controller: return "cpu"
            case .async: return "bolt.horizontal.circle"
            case .audioGeometry: return "slider.horizontal.3"
            case .busReset: return "bolt.horizontal.circle"
            case .logs: return "doc.text"
            case .loggingSettings: return "slider.horizontal.3"
            case .audio: return "hifispeaker.fill"
            case .duet: return "slider.horizontal.below.square.filled.and.square"
#if !ASFW_LITE
            case .audioTelemetry: return "waveform.path.ecg"
            case .dvCapture: return "video.fill"
            case .mcpSettings: return "point.3.connected.trianglepath.dotted"
            case .audioAnalyzer: return "waveform"
#endif
            case .diagnostics: return "heart.text.square"
            case .diceReport: return "doc.text.magnifyingglass"
            case .avcReport: return "doc.text.magnifyingglass"
            }
        }

        var group: SidebarGroup {
            switch self {
            case .overview: .general
#if ASFW_LITE
            case .busInspector, .deviceSettings, .avcUnits, .duet: .devices
#else
            case .busInspector, .deviceSettings, .avcUnits, .duet, .audioAnalyzer: .devices
            case .dvCapture: .video
#endif
            case .diagnostics, .diceReport, .avcReport: .reports
            case .logs, .loggingSettings: .support
            default: .advanced
            }
        }

        var requiresAdvancedTools: Bool { group == .advanced }

        /// Everything but the overview and Core Audio works on one controller.
        var needsController: Bool {
            switch self {
            case .overview, .audio: return false
            default: return true
            }
        }
    }

    private var session: CardSession? { workspace.selectedSession }

    private var visibleSections: [SidebarSection] {
        SidebarSection.allCases.filter { section in
            guard showAdvancedTools || !section.requiresAdvancedTools else { return false }
            if section.needsController && session == nil { return false }
            switch section {
            case .avcUnits, .avcCommands, .avcReport: return session?.debugVM.hasAVCDevice ?? false
            case .diceReport: return session?.debugVM.hasDICEDevice ?? false
            case .duet: return session?.debugVM.hasDuetDevice ?? false
#if ASFW_LITE
            case .audioGeometry:
                return !publishedAudioDevices.isEmpty
#else
            case .audioAnalyzer, .audioTelemetry, .audioGeometry:
                return !publishedAudioDevices.isEmpty
#endif
            default: return true
            }
        }
    }

    private func refreshPublishedAudioDevices() {
        publishedAudioDevices = AudioSystem.shared.devices.filter {
#if ASFW_LITE
            // ASFWAudioObserverClient (analyzer) is not in this build; same rule.
            $0.uid.hasPrefix("ASFW-")
#else
            ASFWAudioObserverClient.guid(fromDeviceUID: $0.uid) != nil
#endif
        }
    }

    var body: some View {
        NavigationSplitView {
            List(selection: $selectedSection) {
                controllersSection
                ForEach(SidebarGroup.allCases) { group in
                    let sections = visibleSections.filter { $0.group == group }
                    if !sections.isEmpty {
                        Section(group.rawValue) {
                            ForEach(sections) { section in
                                Label(section.rawValue, systemImage: section.systemImage)
                                    .tag(section)
                            }
                        }
                    }
                }
            }
            .navigationTitle("ASFW")
            .listStyle(.sidebar)
            .safeAreaInset(edge: .bottom) {
                Toggle("Show Advanced Tools", isOn: $showAdvancedTools)
                    .toggleStyle(.switch)
                    .controlSize(.small)
                    .padding()
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(.bar)
                    .help("Show technical inspection and command tools. Logging is configured separately.")
            }
        } detail: {
            Group {
                detail(for: selectedSection)
            }
            // Switching controllers rebuilds the detail so no view keeps the old card's state.
            .id(workspace.selectedCardID)
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    HStack(spacing: 12) {
                        if driverVM.isBusy {
                            ProgressView()
                                .controlSize(.small)
                        }

                        Button {
                            driverVM.installDriver(requireNewerBuild: requireNewerBuild)
                        } label: {
                            Label("Install", systemImage: "arrow.down.circle.fill")
                        }
                        .labelStyle(.titleAndIcon)
                        .disabled(driverVM.isBusy)
                        .keyboardShortcut("i", modifiers: .command)

                        Button {
                            driverVM.uninstallDriver()
                        } label: {
                            Label("Delete", systemImage: "trash.fill")
                        }
                        .labelStyle(.titleAndIcon)
                        .disabled(driverVM.isBusy)
                        .tint(.red)
                        .keyboardShortcut("u", modifiers: .command)
                    }
                }
            }
        }
        .onAppear {
            workspace.attach(driverVM: driverVM)
        }
        .onDisappear {
            workspace.stopAll()
        }
        .task {
            // Status notifications refresh discovery (DebugViewModel.handleStatusUpdate);
            // this slow poll only catches a discovery that finished after the last one.
            // Each pass is one user-client call per controller, answered from the
            // driver's registry without bus traffic.
            while !Task.isCancelled {
                for session in workspace.sessions.values {
                    session.debugVM.refreshDiscovery()
                }
                refreshPublishedAudioDevices()
                do { try await Task.sleep(nanoseconds: 10_000_000_000) } catch { return }
            }
        }
        .onChange(of: visibleSections) { _, sections in
            if let selectedSection, !sections.contains(selectedSection) {
                self.selectedSection = .overview
            }
        }
    }

    @ViewBuilder
    private var controllersSection: some View {
        Section("Controllers") {
            if workspace.cards.isEmpty {
                Label("No controller driven by ASFW", systemImage: "cable.connector.slash")
                    .foregroundColor(.secondary)
            } else {
                Picker("Controller", selection: $workspace.selectedCardID) {
                    ForEach(workspace.cards) { card in
                        Label(card.displayName, systemImage: "pcicard")
                            .tag(Optional(card.id))
                    }
                }
                .pickerStyle(.inline)
                .labelsHidden()
            }
        }
    }

    @ViewBuilder
    private func detail(for section: SidebarSection?) -> some View {
        switch section {
        case .overview:
            OverviewView(viewModel: driverVM, workspace: workspace,
                         requireNewerBuild: $requireNewerBuild)
        case .audio:
            AudioDebugView()
        case .none:
            Text("Select a section")
                .foregroundStyle(.secondary)
        case .some(let section):
            if let session {
                controllerDetail(section, session: session)
            } else {
                ContentUnavailableView(
                    "No Controller",
                    systemImage: "cable.connector.slash",
                    description: Text("ASFW is not driving a FireWire controller. Install and activate the driver and arm the controller.")
                )
            }
        }
    }

    @ViewBuilder
    private func controllerDetail(_ section: SidebarSection, session: CardSession) -> some View {
        switch section {
        case .busInspector:
            BusInspectorView(debugViewModel: session.debugVM, topologyViewModel: session.topologyVM,
                             romViewModel: session.romExplorerVM)
        case .deviceSettings:
            DeviceSettingsView(viewModel: session.deviceSettingsVM)
        case .avcUnits:
            AvcUnitsView(store: session.avcUnitsStore, connector: session.connector,
                         developerTools: showAdvancedTools)
        case .avcCommands:
            AVCCommandView(viewModel: session.debugVM)
        case .controller:
            ControllerDetailView(viewModel: session.debugVM)
        case .async:
            CommandsView(viewModel: session.debugVM)
#if !ASFW_LITE
        case .audioTelemetry:
            AudioTelemetryView(connector: session.connector)
        case .dvCapture:
            DVCaptureView(viewModel: session.debugVM)
#endif
        case .audioGeometry:
            AudioGeometryView(connector: session.connector)
        case .busReset:
            BusResetHistoryView(viewModel: session.debugVM)
        case .logs:
            SystemLogsView(connector: session.connector)
        case .loggingSettings:
            LoggingSettingsView(connector: session.connector, showAdvancedControls: showAdvancedTools)
#if !ASFW_LITE
        case .mcpSettings:
            MCPSettingsView(viewModel: session.mcpVM)
        case .audioAnalyzer:
            AudioAnalyzerView(devices: publishedAudioDevices, refresh: refreshPublishedAudioDevices)
#endif
        case .duet:
            DuetControlView(connector: session.connector)
        case .diagnostics:
            DiagnosticsView(store: session.diagnosticsStore, showAdvancedTools: showAdvancedTools)
        case .diceReport:
            DiceReportView(store: session.diceReportStore)
        case .avcReport:
            AvcReportView(store: session.avcReportStore)
        case .overview, .audio:
            EmptyView()
        }
    }
}

struct AsyncCommandView: View {
    @ObservedObject var viewModel: DebugViewModel

    @State private var destinationID: String = "0x0000"
    @State private var addressHigh: String = "0x0000"
    @State private var addressLow: String = "0x00000000"
    @State private var readLength: String = "16"
    @State private var payloadHex: String = ""
    @State private var validationError: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                header

                if let error = validationError {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .font(.callout)
                }

                if let message = viewModel.asyncStatusMessage {
                    Label(message, systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                        .font(.callout)
                }

                if let message = viewModel.asyncErrorMessage {
                    Label(message, systemImage: "xmark.octagon.fill")
                        .foregroundStyle(.red)
                        .font(.callout)
                }

                GroupBox("Common Parameters") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                        GridRow {
                            Text("Destination ID")
                            TextField("0x0000", text: $destinationID)
                                .textFieldStyle(.roundedBorder)
                                .monospaced()
                                .frame(width: 160)
                        }
                        GridRow {
                            Text("Address High")
                            TextField("0x0000", text: $addressHigh)
                                .textFieldStyle(.roundedBorder)
                                .monospaced()
                                .frame(width: 160)
                        }
                        GridRow {
                            Text("Address Low")
                            TextField("0x00000000", text: $addressLow)
                                .textFieldStyle(.roundedBorder)
                                .monospaced()
                                .frame(width: 200)
                        }
                    }
                }

                commandSection
            }
            .padding()
        }
        .navigationTitle("Async Commands")
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Asynchronous Request Helpers")
                .font(.title2.bold())
            Text("Send raw async read/write transactions directly through the debug user client. Values accept decimal, hex (0x), octal (0o) or binary (0b).")
                .font(.callout)
                .foregroundStyle(.secondary)

            if !viewModel.isConnected {
                Label("Driver not connected", systemImage: "cable.connector.slash")
                    .foregroundStyle(.orange)
            }
        }
    }

    private var commandSection: some View {
        VStack(alignment: .leading, spacing: 16) {
            GroupBox("Async Read") {
                VStack(alignment: .leading, spacing: 12) {
                    HStack(spacing: 12) {
                        Text("Length")
                        TextField("16", text: $readLength)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 120)
                            .monospaced()
                        Spacer()
                        Button {
                            submitRead()
                        } label: {
                            Label("Issue Read", systemImage: "arrow.down.circle")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(!viewModel.isConnected || viewModel.asyncInProgress)
                    }
                }
            }

            GroupBox("Async Write") {
                VStack(alignment: .leading, spacing: 12) {
                    Text("Payload (hex bytes)")
                        .font(.subheadline)
                    TextEditor(text: $payloadHex)
                        .font(.system(.body, design: .monospaced))
                        .frame(minHeight: 100)
                        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.secondary.opacity(0.2)))

                    HStack {
                        Button("Clear") { payloadHex.removeAll() }
                        Spacer()
                        Button {
                            submitWrite()
                        } label: {
                            Label("Issue Write", systemImage: "arrow.up.circle")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(!viewModel.isConnected || viewModel.asyncInProgress)
                    }
                }
            }

            if viewModel.asyncInProgress {
                ProgressView()
                    .progressViewStyle(.linear)
            }
        }
    }

    private func submitRead() {
        validationError = nil
        guard let destination = parseUInt16(destinationID) else {
            validationError = "Invalid destination ID"
            return
        }
        guard let high = parseUInt16(addressHigh) else {
            validationError = "Invalid address high"
            return
        }
        guard let low = parseUInt32(addressLow) else {
            validationError = "Invalid address low"
            return
        }
        guard let length = parseUInt32(readLength), length > 0 else {
            validationError = "Invalid length"
            return
        }

        viewModel.performAsyncRead(destinationID: destination,
                                   addressHigh: high,
                                   addressLow: low,
                                   length: length)
    }

    private func submitWrite() {
        validationError = nil
        guard let destination = parseUInt16(destinationID) else {
            validationError = "Invalid destination ID"
            return
        }
        guard let high = parseUInt16(addressHigh) else {
            validationError = "Invalid address high"
            return
        }
        guard let low = parseUInt32(addressLow) else {
            validationError = "Invalid address low"
            return
        }
        guard let data = dataFromHex(payloadHex), !data.isEmpty else {
            validationError = "Payload must contain at least one byte"
            return
        }

        viewModel.performAsyncWrite(destinationID: destination,
                                    addressHigh: high,
                                    addressLow: low,
                                    payload: data)
    }

    private func parseUInt16(_ value: String) -> UInt16? {
        parseUnsigned(value)
    }

    private func parseUInt32(_ value: String) -> UInt32? {
        parseUnsigned(value)
    }

    private func parseUnsigned<T: FixedWidthInteger & UnsignedInteger>(_ text: String) -> T? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }

        let value: String
        let radix: Int
        if trimmed.hasPrefix("0x") || trimmed.hasPrefix("0X") {
            value = String(trimmed.dropFirst(2))
            radix = 16
        } else if trimmed.hasPrefix("0b") || trimmed.hasPrefix("0B") {
            value = String(trimmed.dropFirst(2))
            radix = 2
        } else if trimmed.hasPrefix("0o") || trimmed.hasPrefix("0O") {
            value = String(trimmed.dropFirst(2))
            radix = 8
        } else {
            value = trimmed
            radix = 10
        }

        return T(value, radix: radix)
    }

    private func dataFromHex(_ text: String) -> Data? {
        let cleaned = text.replacingOccurrences(of: "\\s", with: "", options: .regularExpression)
        guard !cleaned.isEmpty, cleaned.count % 2 == 0 else { return nil }

        var data = Data(capacity: cleaned.count / 2)
        var index = cleaned.startIndex
        while index < cleaned.endIndex {
            let nextIndex = cleaned.index(index, offsetBy: 2)
            let byteString = cleaned[index..<nextIndex]
            guard let byte = UInt8(byteString, radix: 16) else { return nil }
            data.append(byte)
            index = nextIndex
        }
        return data
    }
}

#if DEBUG
struct AsyncCommandView_Previews: PreviewProvider {
    static var previews: some View {
        AsyncCommandView(viewModel: DebugViewModel())
            .frame(width: 600, height: 600)
    }
}
#endif
