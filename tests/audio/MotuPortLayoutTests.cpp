// SPDX-License-Identifier: Apache-2.0
//
// MotuPortLayout tests.
//
// Pin which jack each host channel reaches. The chunk positions come from FFADO's
// port-group tables (motu_avdevice.cpp:111-121, :135-145), whose packet offsets are
// assigned in array order (motu_avdevice.cpp:1839-1862).

#include "Audio/Wire/MOTU/MotuPortLayout.hpp"
#include "Audio/DriverKit/Config/MOTU/MotuProfile.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

namespace {

using ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
using ASFW::DeviceProfiles::Audio::kMotu896hdSwVersion;
using ASFW::DeviceProfiles::Audio::kMotuUltraliteSwVersion;
using ASFW::Encoding::Motu::ChannelNamesForStream;
using ASFW::Encoding::Motu::ChunkForHostChannel;
using ASFW::Encoding::Motu::EffectivePortMap;
using ASFW::Encoding::Motu::IsChunkPermutation;
using ASFW::Encoding::Motu::MotuPort;
using ASFW::Encoding::Motu::MotuPortMap;
using ASFW::Isoch::Audio::MOTU::Profiles::CapturePortsForSwVersion;
using ASFW::Isoch::Audio::MOTU::Profiles::PlaybackPortsForSwVersion;

constexpr uint32_t kFixedChunks = 14;

TEST(MotuPortLayoutTests, UltraLiteDefaultOutputPairIsTheMainOuts) {
    const MotuPortMap playback = PlaybackPortsForSwVersion(kMotuUltraliteSwVersion);
    ASSERT_EQ(playback.size(), kFixedChunks);
    // Wire chunks 10-11 are Main L/R; chunks 0-1 are the headphone pair.
    EXPECT_EQ(ChunkForHostChannel(playback, 0), 10U);
    EXPECT_EQ(ChunkForHostChannel(playback, 1), 11U);
    EXPECT_EQ(std::string_view(playback[0].name), "Main L");
    EXPECT_EQ(std::string_view(playback[1].name), "Main R");
    EXPECT_EQ(ChunkForHostChannel(playback, 12), 0U);
    EXPECT_EQ(std::string_view(playback[12].name), "Phones L");
}

TEST(MotuPortLayoutTests, UltraLiteDefaultInputPairIsTheMicInputs) {
    const MotuPortMap capture = CapturePortsForSwVersion(kMotuUltraliteSwVersion);
    ASSERT_EQ(capture.size(), kFixedChunks);
    // Wire chunks 2-3 are Mic 1/2; chunks 0-1 are the CueMix return.
    EXPECT_EQ(ChunkForHostChannel(capture, 0), 2U);
    EXPECT_EQ(ChunkForHostChannel(capture, 1), 3U);
    EXPECT_EQ(std::string_view(capture[0].name), "Mic 1");
    EXPECT_EQ(ChunkForHostChannel(capture, 10), 0U);
}

TEST(MotuPortLayoutTests, The828mk2MicsSitAfterItsEightAnalogInputs) {
    const MotuPortMap capture = CapturePortsForSwVersion(kMotu828mk2SwVersion);
    ASSERT_EQ(capture.size(), kFixedChunks);
    EXPECT_EQ(ChunkForHostChannel(capture, 0), 2U);  // Analog 1
    EXPECT_EQ(ChunkForHostChannel(capture, 8), 10U); // Mic 1
    EXPECT_EQ(std::string_view(capture[8].name), "Mic 1");
}

// One name per channel: the fixed ports, then the ADAT channels the optical
// port adds past them, 8 at 1x and 4 at 2x (b146: an ADAT unit's inputs were
// "Input 1".."Input 22", and 22 names stood for 18 channels at 96 kHz).
TEST(MotuPortLayoutTests, The828mk2NamesItsAdatChannelsAfterTheFixedPorts) {
    const MotuPortMap capture = CapturePortsForSwVersion(kMotu828mk2SwVersion);
    const auto at1x = ChannelNamesForStream(capture, kFixedChunks + 8, "ADAT");
    ASSERT_EQ(at1x.size(), kFixedChunks + 8);
    EXPECT_EQ(at1x[0], "Analog 1");
    EXPECT_EQ(at1x[11], "S/PDIF 2");
    EXPECT_EQ(at1x[13], "Mix Return R");
    EXPECT_EQ(at1x[14], "ADAT 1");
    EXPECT_EQ(at1x[21], "ADAT 8");

    const auto at2x = ChannelNamesForStream(capture, kFixedChunks + 4, "ADAT");
    ASSERT_EQ(at2x.size(), kFixedChunks + 4);
    EXPECT_EQ(at2x[17], "ADAT 4");

    const auto fixedOnly = ChannelNamesForStream(capture, kFixedChunks, "ADAT");
    ASSERT_EQ(fixedOnly.size(), kFixedChunks);
    EXPECT_EQ(fixedOnly[13], "Mix Return R");

    const MotuPortMap playback = PlaybackPortsForSwVersion(kMotu828mk2SwVersion);
    const auto outputs = ChannelNamesForStream(playback, kFixedChunks + 8, "ADAT");
    ASSERT_EQ(outputs.size(), kFixedChunks + 8);
    EXPECT_EQ(outputs[0], "Main L");
    EXPECT_EQ(outputs[13], "Phones R");
    EXPECT_EQ(outputs[14], "ADAT 1");
}

// A table that does not fit the stream names nothing, and neither do extra
// chunks a model has no name for: the driver synthesizes names instead.
TEST(MotuPortLayoutTests, NamesFitTheStreamOrAreLeftToTheDriver) {
    const MotuPortMap capture = CapturePortsForSwVersion(kMotu828mk2SwVersion);
    EXPECT_TRUE(ChannelNamesForStream(capture, kFixedChunks - 2, "ADAT").empty());
    EXPECT_TRUE(ChannelNamesForStream(capture, kFixedChunks + 4, nullptr).empty());
    EXPECT_EQ(ChannelNamesForStream(capture, kFixedChunks, nullptr).size(), kFixedChunks);
    EXPECT_TRUE(ChannelNamesForStream(MotuPortMap{}, 8, "ADAT").empty());
}

TEST(MotuPortLayoutTests, EveryModelTableIsAPermutationOfItsChunks) {
    for (const uint32_t sw : {kMotu828mk2SwVersion, kMotuUltraliteSwVersion}) {
        EXPECT_TRUE(IsChunkPermutation(PlaybackPortsForSwVersion(sw))) << sw;
        EXPECT_TRUE(IsChunkPermutation(CapturePortsForSwVersion(sw))) << sw;
    }
}

TEST(MotuPortLayoutTests, UnconfirmedModelsStayInWireOrder) {
    EXPECT_TRUE(PlaybackPortsForSwVersion(kMotu896hdSwVersion).empty());
    EXPECT_TRUE(CapturePortsForSwVersion(kMotu896hdSwVersion).empty());
    EXPECT_EQ(ChunkForHostChannel({}, 5), 5U);
}

TEST(MotuPortLayoutTests, ChannelsPastTheTableKeepTheirWireChunk) {
    // Optical extras follow every fixed chunk, so a 22-chunk ADAT stream maps
    // host channel 14+ straight through.
    const MotuPortMap playback = PlaybackPortsForSwVersion(kMotu828mk2SwVersion);
    const MotuPortMap map = EffectivePortMap(playback, 22);
    EXPECT_EQ(map.size(), kFixedChunks);
    EXPECT_EQ(ChunkForHostChannel(map, 14), 14U);
    EXPECT_EQ(ChunkForHostChannel(map, 21), 21U);
}

TEST(MotuPortLayoutTests, ATableWiderThanTheStreamFallsBackToWireOrder) {
    // Chunk 13 does not exist in a 12-chunk stream, so the table cannot apply.
    const MotuPortMap playback = PlaybackPortsForSwVersion(kMotuUltraliteSwVersion);
    EXPECT_TRUE(EffectivePortMap(playback, 12).empty());
}

TEST(MotuPortLayoutTests, PermutationCheckRejectsDuplicatesAndGaps) {
    constexpr MotuPort duplicate[] = {{"a", 0}, {"b", 0}};
    constexpr MotuPort gap[] = {{"a", 0}, {"b", 2}};
    EXPECT_FALSE(IsChunkPermutation(duplicate));
    EXPECT_FALSE(IsChunkPermutation(gap));
}

} // namespace
