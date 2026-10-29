//
//  DeviceSettingsView.swift
//  ASFW
//
//  Clock source, optical port modes and analog input switches of the
//  controller's MOTU devices.
//

import SwiftUI

struct DeviceSettingsView: View {
    @ObservedObject var viewModel: DeviceSettingsViewModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                Text("Device Settings")
                    .font(.title2.bold())

                if viewModel.devices.isEmpty {
                    ContentUnavailableView(
                        "No Configurable Device",
                        systemImage: "slider.horizontal.3",
                        description: Text("Settings are available for MOTU devices once they are discovered on this controller.")
                    )
                } else {
                    Picker("Device", selection: $viewModel.selectedGUID) {
                        ForEach(viewModel.devices) { device in
                            Text("\(device.name) · \(String(format: "%016llX", device.guid))")
                                .tag(Optional(device.guid))
                        }
                    }
                    .frame(maxWidth: 520)

                    if let snapshot = viewModel.snapshot {
                        settings(snapshot)
                    } else if viewModel.isBusy {
                        ProgressView("Reading device registers…")
                    }

                    if let message = viewModel.message {
                        Text(message)
                            .font(.callout)
                            .foregroundColor(.secondary)
                    }

                    HStack {
                        Button("Reload") { Task { await viewModel.reload() } }
                        if viewModel.isBusy {
                            ProgressView().controlSize(.small)
                        }
                    }
                    .disabled(viewModel.isBusy)
                }

                diagnostics
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle("Device Settings")
        .task {
            viewModel.reloadLogLevel()
            await viewModel.reload()
        }
    }

    /// The driver's log level: for every controller, not only this device.
    private var diagnostics: some View {
        GroupBox("Diagnostics") {
            VStack(alignment: .leading, spacing: 8) {
                if let current = viewModel.logLevel {
                    Picker("Log level", selection: Binding(
                        get: { current.level },
                        set: { level in viewModel.setLogLevel(level) }
                    )) {
                        ForEach(ASFWDriverConnector.logLevels, id: \.self) { level in
                            Text(level == ASFWDriverConnector.defaultLogLevel ? "\(level) (default)" : level).tag(level)
                        }
                    }
                    .frame(maxWidth: 320)
                    Text("warn keeps errors, refusals and the numbered start steps. info adds one line per stream start and stop; debug adds heartbeats and per-step detail, trace every transaction. Raise it to reproduce a problem for a report, then set it back: debug and trace cost CPU while streaming.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    if current.hasOverrides || current.source == .bootProperty || current.source == .commandLine {
                        Text("In force: \(current.spec), set by \(current.source.title).")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                } else {
                    Text("The log level is shown while the driver is connected.")
                        .font(.callout)
                        .foregroundColor(.secondary)
                }
                if let message = viewModel.logLevelMessage {
                    Text(message)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            .padding(6)
            .frame(maxWidth: 520, alignment: .leading)
        }
    }

    private func rememberedTitle(_ setting: DeviceSetting, _ value: UInt32) -> String {
        switch setting {
        case .clockSource: return DeviceClockSource(rawValue: value)?.title ?? "\(value)"
        case .opticalInputMode, .opticalOutputMode: return DeviceOpticalMode(rawValue: value)?.title ?? "\(value)"
        case .inputLevels: return inputsTitle(value, set: "+4 dBu on", none: "−10 dBV on all")
        case .inputBoosts: return inputsTitle(value, set: "on for", none: "off")
        }
    }

    /// "<set> 1, 2, 5" for the analog inputs whose bit is set, `none` without any.
    private func inputsTitle(_ mask: UInt32, set: String, none: String) -> String {
        let inputs = DeviceSettingsSnapshot.inputs(in: mask).map(String.init)
        return inputs.isEmpty ? none : "\(set) \(inputs.joined(separator: ", "))"
    }

    private func inputBit(_ input: Int) -> UInt32 {
        DeviceSettingsSnapshot.mask(0, input: input, set: true)
    }

    @ViewBuilder
    private func settings(_ snapshot: DeviceSettingsSnapshot) -> some View {
        GroupBox("Clock") {
            VStack(alignment: .leading, spacing: 12) {
                if snapshot.supports(.clockSource) {
                    Picker("Clock source", selection: Binding(
                        get: { snapshot.clockSource },
                        set: { value in Task { await viewModel.apply(.clockSource, value: value) } }
                    )) {
                        ForEach(snapshot.clockSourceChoices) { source in
                            Text(source.title).tag(source.rawValue)
                        }
                    }
                    Text("The sample rate follows Core Audio. On an external source the device locks to that signal's rate.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            .padding(6)
            .frame(maxWidth: 520, alignment: .leading)
        }

        if snapshot.supports(.opticalInputMode) || snapshot.supports(.opticalOutputMode) {
            GroupBox("Optical Ports") {
                VStack(alignment: .leading, spacing: 12) {
                    if snapshot.supports(.opticalInputMode) {
                        Picker("Optical in", selection: Binding(
                            get: { snapshot.opticalInputMode },
                            set: { value in Task { await viewModel.apply(.opticalInputMode, value: value) } }
                        )) {
                            ForEach(snapshot.opticalModeChoices) { mode in
                                Text(mode.title).tag(mode.rawValue)
                            }
                        }
                    }
                    if snapshot.supports(.opticalOutputMode) {
                        Picker("Optical out", selection: Binding(
                            get: { snapshot.opticalOutputMode },
                            set: { value in Task { await viewModel.apply(.opticalOutputMode, value: value) } }
                        )) {
                            ForEach(snapshot.opticalModeChoices) { mode in
                                Text(mode.title).tag(mode.rawValue)
                            }
                        }
                    }
                    Text("ADAT adds eight channels per direction at 1x rates; TOSLINK carries stereo S/PDIF. Changing a mode republishes the audio device with the new channel count.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .padding(6)
                .frame(maxWidth: 520, alignment: .leading)
            }
        }

        if snapshot.supports(.inputLevels) || snapshot.supports(.inputBoosts) {
            GroupBox("Analog Inputs") {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(snapshot.switchableInputs, id: \.self) { input in
                        HStack(spacing: 16) {
                            Text("Analog \(input)")
                                .frame(width: 72, alignment: .leading)
                            if snapshot.supports(.inputLevels) {
                                Picker("Analog \(input) level", selection: Binding(
                                    get: { snapshot.inputLevels & inputBit(input) != 0 },
                                    set: { plus4 in
                                        let value = DeviceSettingsSnapshot.mask(snapshot.inputLevels, input: input, set: plus4)
                                        Task { await viewModel.apply(.inputLevels, value: value) }
                                    }
                                )) {
                                    Text("+4 dBu").tag(true)
                                    Text("−10 dBV").tag(false)
                                }
                                .pickerStyle(.segmented)
                                .labelsHidden()
                                .frame(width: 170)
                            }
                            if snapshot.supports(.inputBoosts) {
                                Toggle("Boost +6 dB", isOn: Binding(
                                    get: { snapshot.inputBoosts & inputBit(input) != 0 },
                                    set: { on in
                                        let value = DeviceSettingsSnapshot.mask(snapshot.inputBoosts, input: input, set: on)
                                        Task { await viewModel.apply(.inputBoosts, value: value) }
                                    }
                                ))
                            }
                        }
                    }
                    Text("+4 dBu suits professional line-level sources, −10 dBV consumer ones; Boost adds 6 dB for a quiet source. The unit switches at once, and a running stream is not restarted.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .padding(6)
                .frame(maxWidth: 520, alignment: .leading)
            }
            // Each change is built from the state last read: one at a time.
            .disabled(viewModel.isBusy)
        }

        GroupBox("At Login") {
            VStack(alignment: .leading, spacing: 8) {
                if viewModel.remembered.isEmpty {
                    Text("Nothing remembered for this device yet. A setting changed here is remembered and applied again by the ASFW login agent after a reboot, a re-plug or a power cycle of the device.")
                        .font(.callout)
                        .foregroundColor(.secondary)
                } else {
                    Text("Remembered: " + viewModel.remembered
                        .sorted { $0.key.rawValue < $1.key.rawValue }
                        .map { "\($0.key.title) = \(rememberedTitle($0.key, $0.value))" }
                        .joined(separator: ", "))
                        .font(.callout)
                    Button("Forget") { viewModel.forgetRemembered() }
                }
            }
            .padding(6)
            .frame(maxWidth: 520, alignment: .leading)
        }

        Text(String(format: "Registers: clock 0x%08X · optical 0x%08X", snapshot.clockRaw, snapshot.opticalRaw)
             + (snapshot.analogInputSwitches == 0 ? "" :
                String(format: " · level 0x%08X · boost 0x%08X", snapshot.inputLevelRaw, snapshot.inputBoostRaw)))
            .font(.system(.caption, design: .monospaced))
            .foregroundColor(.secondary)
    }
}
