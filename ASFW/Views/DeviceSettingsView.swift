//
//  DeviceSettingsView.swift
//  ASFW
//
//  Clock source and optical port modes of the controller's MOTU devices.
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
        }
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

        Text(String(format: "Registers: clock 0x%08X · optical 0x%08X", snapshot.clockRaw, snapshot.opticalRaw))
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
                Picker("Hold data back after the start", selection: $viewModel.experiment.extraNoDataPackets) {
                    ForEach(MotuExperiment.dataDelayChoices, id: \.packets) { choice in
                        Text(choice.title).tag(choice.packets)
                    }
                }
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
