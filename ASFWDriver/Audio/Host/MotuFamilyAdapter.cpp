// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuFamilyAdapter.cpp - see MotuFamilyAdapter.hpp.

#include "MotuFamilyAdapter.hpp"

#include "../Protocols/DeviceProtocolChoice.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include "../Protocols/MOTU/MotuRegisters.hpp"
#include "../Protocols/MOTU/MotuNotificationMailbox.hpp"
#include "../Wire/MOTU/MotuBlockLayout.hpp"
#include "../Model/RateConfiguration.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../Logging/Logging.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ASFW::Audio::Host {

Model::ASFWAudioDevice MotuFamilyAdapter::BuildNubConfig(const Discovery::DeviceRecord& record,
                                                         const IDeviceProtocol& protocol) {
    // Geometry combines the model table with completed clock/optical reads.
    // Model identity comes from Unit_Sw_Version; root model_id is generally 0.
    Model::ASFWAudioDevice dev{};
    dev.guid = record.guid;
    dev.vendorId = record.vendorId;
    dev.modelId = record.modelId;
    // CoreAudio shows this in the Sound panel, where MOTU's own driver named the
    // device "MOTU UltraLite". The model constants stay bare; only the display
    // name is qualified here.
    const char* const modelName =
        DeviceProfiles::Audio::AudioDeviceCatalog::MotuModelNameForSwVersion(
            record.unitSwVersion.value_or(0U));
    dev.deviceName = modelName != nullptr
                         ? std::string(DeviceProfiles::Audio::kMotuVendorName) + " " + modelName
                         : protocol.GetName();
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";
    AudioStreamRuntimeCaps caps{};
    if (!protocol.GetRuntimeAudioStreamCaps(caps) || caps.sampleRateHz == 0) return dev;
    dev.inputChannelCount = caps.hostInputPcmChannels;
    dev.outputChannelCount = caps.hostOutputPcmChannels;
    dev.currentSampleRate = caps.sampleRateHz;
    dev.channelCount = std::max(dev.inputChannelCount, dev.outputChannelCount);
    // Room for the widest optical layout, so a mode change re-lays the streams
    // in place instead of needing a new endpoint.
    const auto capacity = Motu::MaximumOpticalPcmChunks(record.unitSwVersion.value_or(0U));
    dev.inputChannelCapacity = capacity.tx;
    dev.outputChannelCapacity = capacity.rx;
    if (const auto formations = protocol.RateFormations(); formations && !formations->empty()) {
        dev.rateFormationCandidates = *formations;
        dev.rateRouteIncarnation = record.deviceIncarnation;
        dev.rateRouteEpoch = record.routeEpoch;
        dev.rateBusGeneration = record.gen.value;
        dev.usesRateFormations = true;
        dev.deviceSampleRates = true;
        dev.sampleRates.clear();
        for (const auto& formation : *formations)
            if (formation.protocolSupported) dev.sampleRates.push_back(formation.sampleRateHz);
        const auto selected = Model::WithRateFormation(dev, dev.currentSampleRate);
        if (selected) dev = *selected;
        else dev.sampleRates.clear();
    }

    // Port names in host channel order, which is not wire order: the encoder and
    // decoder apply the same model table, so these line up with what each channel
    // carries.
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    if (protocol.GetChannelLabels(inNames, outNames)) {
        dev.inputChannelNames = std::move(inNames);
        dev.outputChannelNames = std::move(outNames);
    }
    // The clock source the unit uses, for the HAL clock source control (b146:
    // Audio MIDI Setup showed "Default" while every unit ran on word clock).
    std::vector<std::pair<uint32_t, std::string>> clockChoices;
    uint32_t clockSource = 0;
    if (protocol.GetClockSourceChoices(clockChoices, clockSource)) {
        for (auto& [value, name] : clockChoices) {
            dev.clockSourceValues.push_back(value);
            dev.clockSourceNames.push_back(std::move(name));
        }
        dev.clockSource = clockSource;
    }
    return dev;
}

namespace {

const char* OpticalModeName(uint32_t mode) noexcept {
    switch (static_cast<DeviceOpticalMode>(mode)) {
    case DeviceOpticalMode::Off: return "off";
    case DeviceOpticalMode::Adat: return "ADAT";
    case DeviceOpticalMode::Spdif: return "S/PDIF";
    }
    return "?";
}

const char* SettingName(DeviceSetting setting) noexcept {
    switch (setting) {
    case DeviceSetting::ClockSource: return "clock";
    case DeviceSetting::OpticalInputMode: return "optical in";
    case DeviceSetting::OpticalOutputMode: return "optical out";
    case DeviceSetting::InputLevels: return "input levels";
    case DeviceSetting::InputBoosts: return "input boosts";
    case DeviceSetting::HardwareMonitoring: return "internal mix";
    }
    return "?";
}

struct PendingSetting {
    DeviceSetting setting;
    uint32_t value;
};

// The settings the unit has now that differ from what it left with. Optical
// modes first: they decide the channel layout the geometry read sees.
std::vector<PendingSetting> SettingsToPutBack(const DeviceSettingsSnapshot& now,
                                              const DeviceSettingsSnapshot& left) {
    std::vector<PendingSetting> pending;
    const auto offer = [&](DeviceSetting setting, uint32_t current, uint32_t value) {
        const uint32_t bit = SettingBit(setting);
        if ((now.supportedSettings & bit) != 0 && (left.supportedSettings & bit) != 0 &&
            current != value) {
            pending.push_back({setting, value});
        }
    };
    offer(DeviceSetting::OpticalInputMode, now.opticalInputMode, left.opticalInputMode);
    offer(DeviceSetting::OpticalOutputMode, now.opticalOutputMode, left.opticalOutputMode);
    offer(DeviceSetting::ClockSource, now.clockSource, left.clockSource);
    const uint32_t switches = now.analogInputSwitches & left.analogInputSwitches;
    if (switches != 0) {
        offer(DeviceSetting::InputLevels, now.inputLevelRaw & switches, left.inputLevelRaw & switches);
        offer(DeviceSetting::InputBoosts, now.inputBoostRaw & switches, left.inputBoostRaw & switches);
    }
    offer(DeviceSetting::HardwareMonitoring, now.hardwareMonitoring, left.hardwareMonitoring);
    return pending;
}

// A unit the host has not seen since the driver started comes with whatever
// monitor mix it was left with, by its front panel or another computer. The
// host's applications do the monitoring, so the mix is switched off; the
// setting switches it on again, and a returning unit keeps what it left with.
std::vector<PendingSetting> SettingsForFirstAttach(const DeviceSettingsSnapshot& now) {
    std::vector<PendingSetting> pending;
    if ((now.supportedSettings & SettingBit(DeviceSetting::HardwareMonitoring)) != 0 &&
        now.hardwareMonitoring != 0) {
        pending.push_back({DeviceSetting::HardwareMonitoring, 0});
    }
    return pending;
}

// One describe, with the put-back of a returning unit in front of it. Runs as
// a chain of protocol completions; each step holds the state alive.
class MotuDescribe final : public std::enable_shared_from_this<MotuDescribe> {
public:
    MotuDescribe(const DescribeInput& in, DescribeDone done)
        : record_(in.record),
          builder_(in.policy->plan.profileBuilder),
          protocol_(in.protocol),
          remembered_(in.remembered),
          newPublication_(!in.committed.has_value()),
          done_(std::move(done)) {}

    void Start() {
        if (remembered_ ? remembered_->settings.has_value() : newPublication_) {
            auto self = shared_from_this();
            protocol_->ReadDeviceSettings([self](IOReturn status, DeviceSettingsSnapshot now) {
                if (status == kIOReturnSuccess) {
                    self->pending_ = self->remembered_
                                         ? SettingsToPutBack(now, *self->remembered_->settings)
                                         : SettingsForFirstAttach(now);
                }
                self->PutBackNextSetting();
            });
            return;
        }
        ReadGeometry();
    }

private:
    void PutBackNextSetting() {
        if (nextSetting_ >= pending_.size()) {
            ReadGeometry();
            return;
        }
        const PendingSetting step = pending_[nextSetting_++];
        auto self = shared_from_this();
        protocol_->ApplyDeviceSetting(step.setting, step.value, [self, step](IOReturn status) {
            self->Note(status == kIOReturnSuccess ? "" : "failed ",
                       !self->remembered_ && step.setting == DeviceSetting::HardwareMonitoring
                           ? "internal mix switched off"
                           : SettingName(step.setting));
            self->PutBackNextSetting();
        });
    }

    void ReadGeometry() {
        // The optical config first (asynchronous, as DICE loads its caps).
        auto self = shared_from_this();
        protocol_->EnsureRuntimeStreamGeometry([self](IOReturn geometryStatus) {
            if (geometryStatus != kIOReturnSuccess) {
                // Failed reads never authorize guessed geometry, including first
                // publication. Discovery can retry when the route is usable again.
                self->done_(DescribeRefusal{geometryStatus, MotuFamilyAdapter::kReadFailedReason});
                return;
            }
            self->PutBackRate();
        });
    }

    void PutBackRate() {
        AudioStreamRuntimeCaps caps{};
        const uint32_t rateNow =
            protocol_->GetRuntimeAudioStreamCaps(caps) ? caps.sampleRateHz : 0;
        const uint32_t rateLeft = remembered_ ? remembered_->sampleRateHz : 0;
        if (rateTried_ || rateLeft == 0 || rateNow == 0 || rateNow == rateLeft) {
            ReadSettingsAndFinish();
            return;
        }
        rateTried_ = true;
        auto self = shared_from_this();
        protocol_->ApplySampleRate(rateLeft, [self, rateNow](IOReturn status) {
            char note[40];
            snprintf(note, sizeof(note), "rate from %u Hz%s", rateNow,
                     status == kIOReturnSuccess ? "" : " (refused)");
            self->Note("", note);
            if (status != kIOReturnSuccess) {
                self->ReadSettingsAndFinish();
                return;
            }
            self->ReadGeometry();  // the layout follows the rate
        });
    }

    void ReadSettingsAndFinish() {
        // A refresh of a live endpoint describes from clock and optical alone.
        if (!newPublication_) {
            Finish(nullptr);
            return;
        }
        // An attach also reads the settings: for its log line, and as the
        // protocol's LastDeviceSettings, which the host keeps if the unit leaves.
        auto self = shared_from_this();
        protocol_->ReadDeviceSettings([self](IOReturn status, DeviceSettingsSnapshot settings) {
            self->Finish(status == kIOReturnSuccess ? &settings : nullptr);
        });
    }

    void Finish(const DeviceSettingsSnapshot* settings) {
        auto config = MotuFamilyAdapter::BuildNubConfig(record_, *protocol_);
        // The audio driver selects its stream profile by this id; without
        // it the registry falls back to generic DICE, whose AM824 framing
        // refuses the packed geometry at StartIO.
        config.profileBuilderId = static_cast<uint32_t>(builder_);
        if (config.sampleRates.empty()) {
            done_(DescribeRefusal{kIOReturnUnsupported, "unusable-motu-formation"});
            return;
        }
        if (newPublication_) {
            LogAttach(config, settings);
        }
        done_(std::move(config));
    }

    void Note(const char* prefix, const char* what) {
        if (!changes_.empty()) changes_ += ", ";
        changes_ += prefix;
        changes_ += what;
    }

    // One warn line per attach: what the unit is configured as now, and what
    // was put back (b184 ask: the configuration applied at re-attach).
    void LogAttach(const Model::ASFWAudioDevice& config, const DeviceSettingsSnapshot* settings) const {
        const char* clockName = "?";
        for (size_t index = 0; index < config.clockSourceValues.size() &&
                               index < config.clockSourceNames.size(); ++index) {
            if (config.clockSourceValues[index] == config.clockSource) {
                clockName = config.clockSourceNames[index].c_str();
            }
        }
        const uint32_t switches = settings ? settings->analogInputSwitches : 0;
        const char* outcome = !remembered_ ? "first attach"
                              : changes_.empty() ? "as it left" : "put back";
        ASFW_LOG_WARNING(Audio,
                         "[Attach] %{public}s GUID=0x%016llx: %u Hz, %u in/%u out, clock %{public}s, "
                         "optical %{public}s/%{public}s, levels 0x%02x boosts 0x%02x, internal mix %{public}s; "
                         "%{public}s%{public}s%{public}s",
                         config.deviceName.c_str(), record_.guid, config.currentSampleRate,
                         config.inputChannelCount, config.outputChannelCount, clockName,
                         settings ? OpticalModeName(settings->opticalInputMode) : "?",
                         settings ? OpticalModeName(settings->opticalOutputMode) : "?",
                         settings ? (settings->inputLevelRaw & switches) : 0U,
                         settings ? (settings->inputBoostRaw & switches) : 0U,
                         !settings || (settings->supportedSettings &
                                       SettingBit(DeviceSetting::HardwareMonitoring)) == 0
                             ? "?"
                             : settings->hardwareMonitoring != 0 ? "on" : "off",
                         outcome,
                         changes_.empty() ? "" : ": ", changes_.c_str());
    }

    const Discovery::DeviceRecord record_;
    const DeviceProfiles::Audio::ProfileBuilderId builder_;
    const std::shared_ptr<IDeviceProtocol> protocol_;
    const std::optional<RememberedConfiguration> remembered_;
    const bool newPublication_;
    DescribeDone done_;
    std::vector<PendingSetting> pending_;
    size_t nextSetting_{0};
    bool rateTried_{false};
    std::string changes_;
};

} // namespace

void MotuFamilyAdapter::Describe(const DescribeInput& in, DescribeDone done) {
    // The host only routes MOTU devices here; refuse anything else rather than
    // publish a MOTU description for another family's device.
    if (!in.policy || ChooseAudioBackend(in.policy->plan) != AudioBackendKind::MotuRegister) {
        done(DescribeRefusal{kIOReturnUnsupported, "not-a-motu-policy"});
        return;
    }
    // MotuAudioBackend published only once a protocol existed; the next record
    // update or restart describes again.
    if (!in.protocol) {
        done(DescribeRefusal{kIOReturnNotReady, "no-protocol"});
        return;
    }
    // A unit that comes back is put back to the rate, layout and settings it
    // left with before it is described, so Core Audio sees the device that left
    // (the user's requirement after b184: configured whenever plugged in).
    std::make_shared<MotuDescribe>(in, std::move(done))->Start();
}

MotuFamilyAdapter::~MotuFamilyAdapter() noexcept {
    Motu::Notifications::ClearObserver(this);
}

void MotuFamilyAdapter::SetEventSink(DeviceEventSink* sink) noexcept {
    if (sink != nullptr) {
        sink_.store(sink, std::memory_order_release);
        Motu::Notifications::SetObserver(this, &MotuFamilyAdapter::NotificationThunk);
        return;
    }
    Motu::Notifications::ClearObserver(this);
    sink_.store(nullptr, std::memory_order_release);
}

void MotuFamilyAdapter::OnUnsolicitedNotification(uint64_t guid, uint32_t bits,
                                                  bool clockChanged) noexcept {
    DeviceEventSink* sink = sink_.load(std::memory_order_acquire);
    if (sink == nullptr || !clockChanged) return;
    sink->OnDeviceEvent(guid, DeviceEvent::kClockStatusChanged, bits);
}

void MotuFamilyAdapter::NotificationThunk(void* context, uint64_t guid, uint32_t bits,
                                          bool clockChanged) noexcept {
    if (auto* self = static_cast<MotuFamilyAdapter*>(context)) {
        self->OnUnsolicitedNotification(guid, bits, clockChanged);
    }
}

} // namespace ASFW::Audio::Host
