#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Audio/Core/AudioEndpointRuntime.hpp"
#include "Audio/Config/AudioConstants.hpp"

namespace {

ASFW::Audio::Model::ASFWAudioDevice MakeDeviceConfig() {
    ASFW::Audio::Model::ASFWAudioDevice config{};
    config.guid = 0x1020304050607080ULL;
    config.channelCount = 8;
    config.inputChannelCount = 6;
    config.outputChannelCount = 4;
    config.currentSampleRate = 48000;
    return config;
}

} // namespace

TEST(AudioEndpointRuntime, MissingConfigDoesNotPublishBinding) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);

    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
    EXPECT_FALSE(runtime.CopyDirectAudioBinding(snapshot));
    EXPECT_FALSE(snapshot.valid);
    EXPECT_FALSE(runtime.IsTransmitTimingReady());
}

TEST(AudioEndpointRuntime, BadCopyArgsZeroOutputs) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    runtime.UpdateConfig(MakeDeviceConfig());

    IOMemoryDescriptor* inputMemory = reinterpret_cast<IOMemoryDescriptor*>(0x1);
    IOMemoryDescriptor* controlMemory = reinterpret_cast<IOMemoryDescriptor*>(0x2);
    uint32_t outputFrames = 99;
    uint32_t outputChannels = 98;
    uint32_t inputFrames = 97;
    uint32_t inputChannels = 96;
    uint32_t sampleRateHz = 95;
    uint64_t generation = 94;

    const kern_return_t kr = runtime.CopyDirectAudioMemory(nullptr,
                                                           &inputMemory,
                                                           &controlMemory,
                                                           &outputFrames,
                                                           &outputChannels,
                                                           &inputFrames,
                                                           &inputChannels,
                                                           &sampleRateHz,
                                                           &generation);

    EXPECT_EQ(kr, kIOReturnBadArgument);
    EXPECT_EQ(inputMemory, nullptr);
    EXPECT_EQ(controlMemory, nullptr);
    EXPECT_EQ(outputFrames, 0u);
    EXPECT_EQ(outputChannels, 0u);
    EXPECT_EQ(inputFrames, 0u);
    EXPECT_EQ(inputChannels, 0u);
    EXPECT_EQ(sampleRateHz, 0u);
    EXPECT_EQ(generation, 0u);
}

// The unit's rate from its own timestamps (frames over SPH ticks), for the
// start sequence's rate check: only this stream generation, and only once
// enough frames have arrived.
TEST(AudioEndpointRuntime, MeasuredDeviceRateComesFromTheUnitsTimestamps) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    runtime.UpdateConfig(MakeDeviceConfig());
    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0, outputChannels = 0, inputFrames = 0, inputChannels = 0, sampleRateHz = 0;
    uint64_t generation = 0;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(&outputMemory, &inputMemory, &controlMemory, &outputFrames,
                                            &outputChannels, &inputFrames, &inputChannels, &sampleRateHz,
                                            &generation),
              kIOReturnSuccess);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(snapshot));
    auto& control = *snapshot.control;
    control.ResetForStart();
    const uint64_t stream = control.generation.load();

    uint32_t rate = 0;
    EXPECT_FALSE(runtime.MeasuredDeviceRateHz(rate));  // nothing published yet

    // Too few frames to decide.
    ASSERT_TRUE(control.motuRxSphClock.Publish({.streamGeneration = stream, .rxFrames = 1000,
                                                .rxTicks = 1000LL * 512}));
    EXPECT_FALSE(runtime.MeasuredDeviceRateHz(rate));

    // 512 ticks per frame is 48 kHz, whatever rate the stream was set to.
    ASSERT_TRUE(control.motuRxSphClock.Publish({.streamGeneration = stream, .rxFrames = 4096,
                                                .rxTicks = 4096LL * 512}));
    ASSERT_TRUE(runtime.MeasuredDeviceRateHz(rate));
    EXPECT_EQ(rate, 48000U);

    // A new start forgets the last stream's measurement.
    control.ResetForStart();
    EXPECT_FALSE(runtime.MeasuredDeviceRateHz(rate));

    outputMemory->release();
    inputMemory->release();
    controlMemory->release();
}

TEST(AudioEndpointRuntime, CopyDirectAudioMemoryAllocatesCompleteDuplexBinding) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    runtime.UpdateConfig(MakeDeviceConfig());

    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0;
    uint32_t outputChannels = 0;
    uint32_t inputFrames = 0;
    uint32_t inputChannels = 0;
    uint32_t sampleRateHz = 0;
    uint64_t generation = 0;

    const kern_return_t kr = runtime.CopyDirectAudioMemory(&outputMemory,
                                                           &inputMemory,
                                                           &controlMemory,
                                                           &outputFrames,
                                                           &outputChannels,
                                                           &inputFrames,
                                                           &inputChannels,
                                                           &sampleRateHz,
                                                           &generation);

    ASSERT_EQ(kr, kIOReturnSuccess);
    ASSERT_NE(outputMemory, nullptr);
    ASSERT_NE(inputMemory, nullptr);
    ASSERT_NE(controlMemory, nullptr);
    // V3: the published frames are the ACTIVE ring of the rate (the ZTS
    // period); the memory itself is allocated once at the maximum ring.
    const uint32_t activeRing =
        ASFW::IsochTransport::HalBufferProfileForRate(48000).frameRingFrames;
    EXPECT_EQ(outputFrames, activeRing);
    EXPECT_EQ(inputFrames, activeRing);
    uint64_t outputBytes = 0;
    uint64_t inputBytes = 0;
    ASSERT_EQ(outputMemory->GetLength(&outputBytes), kIOReturnSuccess);
    ASSERT_EQ(inputMemory->GetLength(&inputBytes), kIOReturnSuccess);
    EXPECT_EQ(outputBytes,
              uint64_t{ASFW::Isoch::Config::kAudioOutputRingFrames} * 4u * sizeof(float));
    EXPECT_EQ(inputBytes,
              uint64_t{ASFW::Isoch::Config::kAudioRingBufferFrames} * 6u * sizeof(int32_t));
    EXPECT_EQ(outputChannels, 4u);
    EXPECT_EQ(inputChannels, 6u);
    EXPECT_EQ(sampleRateHz, 48000u);
    EXPECT_GT(generation, 0u);

    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(snapshot));
    EXPECT_TRUE(snapshot.IsValidDuplex());
    EXPECT_EQ(snapshot.outputFrames, outputFrames);
    EXPECT_EQ(snapshot.outputChannels, outputChannels);
    EXPECT_EQ(snapshot.inputFrames, inputFrames);
    EXPECT_EQ(snapshot.inputChannels, inputChannels);
    EXPECT_EQ(snapshot.sampleRateHz, sampleRateHz);
    EXPECT_EQ(snapshot.generation, generation);
    EXPECT_FALSE(runtime.IsTransmitTimingReady());
    snapshot.control->transmitTimingReady.store(1, std::memory_order_release);
    EXPECT_TRUE(runtime.IsTransmitTimingReady());
    snapshot.control->ResetForStart();
    EXPECT_FALSE(runtime.IsTransmitTimingReady());

    outputMemory->release();
    inputMemory->release();
    controlMemory->release();
}

TEST(AudioEndpointRuntime, PlaybackOnlyPresentationKeepsPhysicalReturnRingForDuplexTransport) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    auto config = MakeDeviceConfig();
    config.channelCount = 2;
    config.inputChannelCount = 0;  // CoreAudio-visible topology.
    config.outputChannelCount = 2;
    runtime.UpdateConfig(config);

    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0;
    uint32_t outputChannels = 0;
    uint32_t inputFrames = 0;
    uint32_t inputChannels = 0;
    uint32_t sampleRateHz = 0;
    uint64_t generation = 0;

    ASSERT_EQ(runtime.CopyDirectAudioMemory(&outputMemory,
                                            &inputMemory,
                                            &controlMemory,
                                            &outputFrames,
                                            &outputChannels,
                                            &inputFrames,
                                            &inputChannels,
                                            &sampleRateHz,
                                            &generation),
              kIOReturnSuccess);
    EXPECT_EQ(outputChannels, 2U);
    EXPECT_EQ(inputChannels, 2U);

    outputMemory->release();
    inputMemory->release();
    controlMemory->release();
}

// A rate change moves the active ring inside the unchanged allocation: the same
// descriptors are republished with the new ring and a bumped generation, so no
// memory is replaced under CoreAudio (V3, TIMING_GEOMETRY_OWNERSHIP.md).
TEST(AudioEndpointRuntime, RateChangeMovesActiveRingAndReusesMemory) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    runtime.UpdateConfig(MakeDeviceConfig());

    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0;
    uint32_t outputChannels = 0;
    uint32_t inputFrames = 0;
    uint32_t inputChannels = 0;
    uint32_t sampleRateHz = 0;
    uint64_t generation = 0;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(&outputMemory, &inputMemory, &controlMemory,
                                            &outputFrames, &outputChannels, &inputFrames,
                                            &inputChannels, &sampleRateHz, &generation),
              kIOReturnSuccess);
    EXPECT_EQ(outputFrames, 12288u);

    runtime.SetCurrentSampleRate(96000);

    ASFW::Audio::Runtime::DirectAudioBindingSnapshot at96{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(at96));
    EXPECT_EQ(at96.sampleRateHz, 96000u);
    EXPECT_EQ(at96.outputFrames, 24576u);
    EXPECT_EQ(at96.inputFrames, 24576u);
    EXPECT_GT(at96.generation, generation);

    IOMemoryDescriptor* outputMemory2 = nullptr;
    IOMemoryDescriptor* inputMemory2 = nullptr;
    IOMemoryDescriptor* controlMemory2 = nullptr;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(&outputMemory2, &inputMemory2, &controlMemory2,
                                            &outputFrames, &outputChannels, &inputFrames,
                                            &inputChannels, &sampleRateHz, &generation),
              kIOReturnSuccess);
    EXPECT_EQ(outputMemory2, outputMemory);
    EXPECT_EQ(inputMemory2, inputMemory);
    EXPECT_EQ(outputFrames, 24576u);
    EXPECT_EQ(sampleRateHz, 96000u);

    // Back to 48 kHz: the ring shrinks again inside the same allocation.
    runtime.SetCurrentSampleRate(48000);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot at48{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(at48));
    EXPECT_EQ(at48.outputFrames, 12288u);
    EXPECT_GT(at48.generation, at96.generation);

    for (IOMemoryDescriptor* memory : {outputMemory, inputMemory, controlMemory, outputMemory2,
                                       inputMemory2, controlMemory2}) {
        memory->release();
    }
}

// The receive path checks the binding's generation on every batch without the
// lock: it must be what a copy would report, and 0 with no complete binding.
TEST(AudioEndpointRuntime, PeekedBindingGenerationMatchesTheCopy) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    EXPECT_EQ(runtime.PeekDirectAudioBindingGeneration(), 0U);
    runtime.UpdateConfig(MakeDeviceConfig());
    EXPECT_EQ(runtime.PeekDirectAudioBindingGeneration(), 0U);

    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0, outputChannels = 0, inputFrames = 0, inputChannels = 0, sampleRateHz = 0;
    uint64_t generation = 0;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(&outputMemory, &inputMemory, &controlMemory, &outputFrames,
                                            &outputChannels, &inputFrames, &inputChannels, &sampleRateHz,
                                            &generation),
              kIOReturnSuccess);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(snapshot));
    EXPECT_NE(snapshot.generation, 0U);
    EXPECT_EQ(runtime.PeekDirectAudioBindingGeneration(), snapshot.generation);

    runtime.SetCurrentSampleRate(96000);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot at96{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(at96));
    EXPECT_GT(at96.generation, snapshot.generation);
    EXPECT_EQ(runtime.PeekDirectAudioBindingGeneration(), at96.generation);

    for (IOMemoryDescriptor* memory : {outputMemory, inputMemory, controlMemory}) {
        memory->release();
    }
}

TEST(AudioEndpointRuntime, AvcRateAndWidthChangesKeepMemoryObjectsAndResetEpoch) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    auto config = MakeDeviceConfig();
    ASFW::Audio::Runtime::RateFormation low{};
    low.sampleRateHz = 48000;
    low.protocolSupported = true;
    low.playback = {{4, 4, 0, {}}};
    low.capture = {{6, 6, 0, {}}};
    auto high = low;
    high.sampleRateHz = 192000;
    high.playback = {{2, 2, 0, {}}};
    high.capture = high.playback;
    config.rateFormationCandidates = {low, high};
    runtime.UpdateConfig(config);
    ASSERT_EQ(runtime.EnsureDirectAudioMemory(), kIOReturnSuccess);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot prior{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(prior));
    config.currentSampleRate = 192000;
    config.inputChannelCount = config.outputChannelCount = 2;
    runtime.UpdateConfig(config);
    runtime.MarkStreaming(true);
    EXPECT_EQ(runtime.EnsureDirectAudioMemory(), kIOReturnBusy);
    runtime.MarkStreaming(false);
    ASSERT_EQ(runtime.EnsureDirectAudioMemory(), kIOReturnSuccess);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot highView{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(highView));
    EXPECT_EQ(prior.outputBase, highView.outputBase);
    EXPECT_EQ(prior.inputBase, highView.inputBase);
    EXPECT_EQ(prior.control, highView.control);
    EXPECT_GT(highView.generation, prior.generation);
    EXPECT_EQ(highView.sampleRateHz, 192000U);
    EXPECT_EQ(highView.outputFrames, 49152U);
    EXPECT_EQ(highView.outputChannels, 2U);
    config.currentSampleRate = 48000;
    config.inputChannelCount = 6;
    config.outputChannelCount = 4;
    runtime.UpdateConfig(config);
    ASSERT_EQ(runtime.EnsureDirectAudioMemory(), kIOReturnSuccess);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot returned{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(returned));
    EXPECT_EQ(returned.outputBase, prior.outputBase);
    EXPECT_EQ(returned.inputBase, prior.inputBase);
    EXPECT_EQ(returned.outputFrames, 12288U);
    EXPECT_EQ(returned.outputChannels, 4U);
}

TEST(AudioEndpointRuntime, PackedCaptureRetains34ChannelsAcrossRateChangesInOneAllocation) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    auto config = MakeDeviceConfig();
    ASFW::Audio::Runtime::RateFormation low{};
    low.sampleRateHz = 48000; low.protocolSupported = true; low.packedPcm = true;
    low.capture = {{34,28}}; low.playback = {{30,25}};
    auto high = low; high.sampleRateHz = 96000;
    high.capture = {{26,22}}; high.playback = {{22,19}};
    config.rateFormationCandidates = {low,high};
    config.currentSampleRate = 48000; config.inputChannelCount = config.channelCount = 34;
    config.outputChannelCount = 30; runtime.UpdateConfig(config);
    ASSERT_EQ(runtime.EnsureDirectAudioMemory(),kIOReturnSuccess);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot first{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(first)); EXPECT_EQ(first.inputChannels,34U);
    config.currentSampleRate = 96000; config.inputChannelCount = config.channelCount = 26;
    config.outputChannelCount = 22; runtime.UpdateConfig(config);
    ASSERT_EQ(runtime.EnsureDirectAudioMemory(),kIOReturnSuccess);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot second{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(second)); EXPECT_EQ(second.inputChannels,26U);
    EXPECT_EQ(second.inputBase,first.inputBase);
    config.currentSampleRate = 48000; config.inputChannelCount = config.channelCount = 34;
    config.outputChannelCount = 30; runtime.UpdateConfig(config);
    ASSERT_EQ(runtime.EnsureDirectAudioMemory(),kIOReturnSuccess);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot last{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(last)); EXPECT_EQ(last.inputChannels,34U);
    EXPECT_EQ(last.inputBase,first.inputBase);
}

namespace {
// An 828mkII's formations for one optical setting: 48 and 96 kHz.
std::vector<ASFW::Audio::Runtime::RateFormation> MotuFormations(uint32_t in48, uint32_t in96,
                                                                 uint32_t out48, uint32_t out96) {
    // Packed: one SPH quadlet, then two message chunks and the PCM chunks.
    const auto dbs = [](uint32_t chunks) { return 1U + ((2U + chunks) * 3U + 3U) / 4U; };
    ASFW::Audio::Runtime::RateFormation low{};
    low.sampleRateHz = 48000;
    low.protocolSupported = true;
    low.packedPcm = true;
    low.capture = {{in48, dbs(in48), 0, {}}};
    low.playback = {{out48, dbs(out48), 0, {}}};
    auto high = low;
    high.sampleRateHz = 96000;
    high.capture = {{in96, dbs(in96), 0, {}}};
    high.playback = {{out96, dbs(out96), 0, {}}};
    return {low, high};
}
} // namespace

// b214 part B: an 828mkII published with S/PDIF optical (14 in) and then
// switched to ADAT input (18 in at 96 kHz). Without the capacity the memory
// held 14 channels and the new layout was refused with NoResources; with it,
// the switch re-lays the binding inside the same memory objects.
TEST(AudioEndpointRuntime, AnOpticalSwitchFitsTheCapacityAllocatedAtPublication) {
    for (const bool withCapacity : {false, true}) {
        SCOPED_TRACE(withCapacity ? "with capacity" : "without capacity");
        ASFW::Audio::AudioEndpointRuntime runtime(0x0001f2000001f717ULL);
        auto config = MakeDeviceConfig();
        config.currentSampleRate = 96000;
        config.inputChannelCount = config.outputChannelCount = config.channelCount = 14;
        config.rateFormationCandidates = MotuFormations(14, 14, 14, 14);
        if (withCapacity) config.inputChannelCapacity = config.outputChannelCapacity = 22;
        runtime.UpdateConfig(config);
        ASSERT_EQ(runtime.EnsureDirectAudioMemory(), kIOReturnSuccess);
        ASFW::Audio::Runtime::DirectAudioBindingSnapshot spdif{};
        ASSERT_TRUE(runtime.CopyDirectAudioBinding(spdif));

        config.inputChannelCount = config.channelCount = 18;
        config.rateFormationCandidates = MotuFormations(22, 18, 14, 14);
        runtime.UpdateConfig(config);
        if (!withCapacity) {
            EXPECT_EQ(runtime.EnsureDirectAudioMemory(), kIOReturnNoResources);
            continue;
        }
        ASSERT_EQ(runtime.EnsureDirectAudioMemory(), kIOReturnSuccess);
        ASFW::Audio::Runtime::DirectAudioBindingSnapshot adat{};
        ASSERT_TRUE(runtime.CopyDirectAudioBinding(adat));
        EXPECT_EQ(adat.inputBase, spdif.inputBase);
        EXPECT_EQ(adat.outputBase, spdif.outputBase);
        EXPECT_EQ(adat.control, spdif.control);
        EXPECT_EQ(adat.inputChannels, 18U);
        EXPECT_GT(adat.generation, spdif.generation);

        // And at 48 kHz, the widest layout: 22 inputs.
        config.currentSampleRate = 48000;
        config.inputChannelCount = config.channelCount = 22;
        runtime.UpdateConfig(config);
        ASSERT_EQ(runtime.EnsureDirectAudioMemory(), kIOReturnSuccess);
        ASFW::Audio::Runtime::DirectAudioBindingSnapshot wide{};
        ASSERT_TRUE(runtime.CopyDirectAudioBinding(wide));
        EXPECT_EQ(wide.inputBase, spdif.inputBase);
        EXPECT_EQ(wide.inputChannels, 22U);
    }
}

