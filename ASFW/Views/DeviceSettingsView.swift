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
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle("Device Settings")
        .task { await viewModel.reload() }
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

        GroupBox("Diagnostics: stream start") {
            VStack(alignment: .leading, spacing: 10) {
                Text("For finding out why the 828mkII resets the bus when playback starts. Each setting changes the next playback start only; the driver logs what it did. Turn everything off when done.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                Toggle("Write fetch-enable before the start (Linux skips it on the 828mkII)",
                       isOn: $viewModel.experiment.forceFetchEnable)
                Toggle("Capture only: do not send any packets to the device",
                       isOn: $viewModel.experiment.captureOnly)
                Toggle("Transmit only: send data but do not receive (SPH free-runs, audio muted)",
                       isOn: $viewModel.experiment.transmitOnly)
                Toggle("Prime with empty CIP packets as before b110 (the default is now skip cycles, like Linux)",
                       isOn: $viewModel.experiment.emptyCipPrefill)
                Toggle("Count DBC like the 828mkII does (data: first block, empty: next packet)",
                       isOn: $viewModel.experiment.startEventDbc)
                Picker("Hold data back after the start", selection: $viewModel.experiment.extraNoDataPackets) {
                    ForEach(MotuExperiment.dataDelayChoices, id: \.packets) { choice in
                        Text(choice.title).tag(choice.packets)
                    }
                }
                Picker("SPH presentation lead", selection: $viewModel.experiment.presentationLeadCycles) {
                    ForEach(MotuExperiment.leadChoices, id: \.cycles) { choice in
                        Text(choice.title).tag(choice.cycles)
                    }
                }
                Toggle("Hold absolute SPH phase at the 828mk3 setpoint (pre-b98 behaviour)",
                       isOn: $viewModel.experiment.holdAbsolutePhase)
                HStack {
                    Button("Send to driver") { viewModel.applyExperiment() }
                    if let message = viewModel.experimentMessage {
                        Text(message).font(.caption).foregroundColor(.secondary)
                    }
                }
            }
            .padding(6)
            .frame(maxWidth: 520, alignment: .leading)
        }
    }
}
