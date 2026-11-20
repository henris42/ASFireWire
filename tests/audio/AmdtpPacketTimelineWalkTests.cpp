// Unit tests for the TX timeline's packet-index walk
// (AmdtpPacketTimeline::SnapshotSlotForAudioFrameNear) and for the payload
// writers that use it: from any hint the walk returns what the full scan
// returns, misses are decided at the ring's edges, and both writers produce
// the same packet bytes and counters as the per-frame scan they replaced.

#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"
#include "Audio/Wire/MOTU/MotuBlockCodec.hpp"
#include "Audio/Wire/MOTU/MotuPayloadWriter.hpp"
#include "Audio/Wire/MOTU/MotuPortLayout.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace {

using ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline;
using ASFW::Protocols::Audio::AMDTP::AmdtpPayloadWriter;
using ASFW::Protocols::Audio::AMDTP::AmdtpStreamConfig;
using ASFW::Protocols::Audio::AMDTP::AmdtpTxPolicy;
using ASFW::Protocols::Audio::AMDTP::HostAudioBufferView;
using ASFW::Protocols::Audio::AMDTP::PacketSlotSnapshot;
using ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot;
using ASFW::Protocols::Audio::AMDTP::PcmSlotCodec;
using ASFW::Protocols::Audio::AMDTP::PreparedTxPacket;

constexpr uint32_t kCipHeaderBytes = 8;

// A slot ring prepared the way the producer prepares it: blocking cadence
// D,D,D,N, each DATA packet carrying the next framesPerPacket frames.
struct Ring {
    AmdtpPacketTimeline timeline{};
    std::vector<PacketTimelineSlot> slots;
    std::vector<uint8_t> bytes;
    uint32_t framesPerPacket;
    uint32_t dbs;
    uint32_t stride;
    uint64_t nextPacket{0};
    uint64_t nextFrame{0};

    Ring(uint32_t slotCount, uint32_t framesPerPacketIn, uint32_t dbsIn)
        : slots(slotCount),
          framesPerPacket(framesPerPacketIn),
          dbs(dbsIn),
          stride(kCipHeaderBytes + framesPerPacketIn * dbsIn * 4U) {
        bytes.assign(static_cast<size_t>(slotCount) * stride, 0xEEu);
        EXPECT_TRUE(timeline.AttachSlots(slots.data(), slotCount));
    }

    static bool IsData(uint64_t packet) { return packet % 4 != 3; }

    void Prepare(uint64_t count) {
        for (uint64_t i = 0; i < count; ++i, ++nextPacket) {
            if (!IsData(nextPacket)) {
                timeline.MarkNoDataPacket(nextPacket);
                continue;
            }
            PreparedTxPacket packet{};
            packet.packetIndex = nextPacket;
            packet.isData = true;
            packet.firstAudioFrame = nextFrame;
            packet.framesInPacket = framesPerPacket;
            packet.dbs = dbs;
            packet.byteCount = stride;
            uint8_t* slotBytes = bytes.data() + (nextPacket % slots.size()) * stride;
            ASSERT_TRUE(timeline.ExposeDataPacket(packet, slotBytes, stride));
            nextFrame += framesPerPacket;
        }
    }

    // A frame-cursor jump between two packets, as a re-anchor makes one.
    void JumpFrames(int64_t delta) { nextFrame = static_cast<uint64_t>(nextFrame + delta); }
};

void ExpectSameAsScan(const AmdtpPacketTimeline& timeline, uint64_t frame, uint64_t hint) {
    PacketSlotSnapshot scanned{};
    PacketSlotSnapshot walked{};
    const bool scanFound = timeline.SnapshotSlotForAudioFrame(frame, scanned);
    const bool walkFound = timeline.SnapshotSlotForAudioFrameNear(frame, hint, walked);
    ASSERT_EQ(walkFound, scanFound) << "frame " << frame << " hint " << hint;
    if (scanFound) {
        EXPECT_EQ(walked.packetIndex, scanned.packetIndex) << "frame " << frame << " hint " << hint;
        EXPECT_EQ(walked.firstAudioFrame, scanned.firstAudioFrame);
        EXPECT_EQ(walked.packetBytes, scanned.packetBytes);
        EXPECT_EQ(walked.slot, scanned.slot);
        EXPECT_EQ(walked.generation, scanned.generation);
    }
}

TEST(AmdtpPacketTimelineWalkTests, FindsEveryFrameFromAnyHint) {
    Ring ring{64, 8, 4};
    ring.Prepare(100);  // wrapped: packets 36..99 are live
    const uint64_t firstLive = ring.nextPacket - ring.slots.size();
    // Frames from before the live window through past the exposed end.
    for (uint64_t frame = 0; frame < ring.nextFrame + 40; ++frame) {
        for (uint64_t hint : {firstLive, firstLive + 7, ring.nextPacket - 1, ring.nextPacket,
                              ring.nextPacket + 30, uint64_t{3}, uint64_t{0}, frame / 6,
                              frame / 6 + 1, frame / 6 + 9}) {
            ExpectSameAsScan(ring.timeline, frame, hint);
        }
    }
}

TEST(AmdtpPacketTimelineWalkTests, ResolvesTheNextPacketInOneStep) {
    Ring ring{64, 8, 4};
    ring.Prepare(40);
    PacketSlotSnapshot snap{};
    // Packet 3 is NO-DATA: from it, frame 24 (packet 4's first) is one step on.
    ASSERT_TRUE(ring.timeline.SnapshotSlotForAudioFrameNear(24, 3, snap));
    EXPECT_EQ(snap.packetIndex, 4U);
    ASSERT_TRUE(ring.timeline.SnapshotSlotForAudioFrameNear(23, 4, snap));
    EXPECT_EQ(snap.packetIndex, 2U);
}

TEST(AmdtpPacketTimelineWalkTests, MissesPastTheExposedEndAndBeforeTheOldestLap) {
    Ring ring{64, 8, 4};
    ring.Prepare(200);
    PacketSlotSnapshot snap{};
    // Past the exposed end: not prepared yet.
    EXPECT_FALSE(ring.timeline.SnapshotSlotForAudioFrameNear(ring.nextFrame, ring.nextPacket - 1, snap));
    EXPECT_FALSE(ring.timeline.SnapshotSlotForAudioFrameNear(ring.nextFrame + 500, 0, snap));
    // Older than one lap: overwritten.
    EXPECT_FALSE(ring.timeline.SnapshotSlotForAudioFrameNear(10, ring.nextPacket - 1, snap));
    EXPECT_FALSE(ring.timeline.SnapshotSlotForAudioFrameNear(10, 2, snap));
}

TEST(AmdtpPacketTimelineWalkTests, AHintFromAnotherStreamStillFindsTheFrame) {
    Ring ring{64, 8, 4};
    ring.Prepare(50);
    for (uint64_t hint : {uint64_t{1'000'000}, uint64_t{999'983}, uint64_t{1u << 31}}) {
        ExpectSameAsScan(ring.timeline, 100, hint);
        ExpectSameAsScan(ring.timeline, 0, hint);
    }
}

TEST(AmdtpPacketTimelineWalkTests, ASlotMidRewriteFallsBackToTheScan) {
    Ring ring{64, 8, 4};
    ring.Prepare(40);
    // Packet 10 is being rewritten: its generation is odd.
    PacketTimelineSlot& busy = ring.slots[10];
    busy.generation.fetch_add(1);
    for (uint64_t frame = 0; frame < ring.nextFrame; ++frame) {
        ExpectSameAsScan(ring.timeline, frame, 10);
        ExpectSameAsScan(ring.timeline, frame, 9);
        ExpectSameAsScan(ring.timeline, frame, 30);
    }
    busy.generation.fetch_add(1);
}

TEST(AmdtpPacketTimelineWalkTests, FramesOutOfPacketOrderAgreeWithTheScan) {
    // A frame-cursor jump forward leaves frames no packet carries; a jump back
    // makes two packets carry the same frames. Wherever the scan gives one
    // answer the walk must give it too.
    Ring forward{64, 8, 4};
    forward.Prepare(20);
    forward.JumpFrames(100);
    forward.Prepare(20);
    for (uint64_t frame = 0; frame < forward.nextFrame + 8; ++frame) {
        for (uint64_t hint : {uint64_t{0}, uint64_t{19}, uint64_t{20}, uint64_t{39}}) {
            ExpectSameAsScan(forward.timeline, frame, hint);
        }
    }

    Ring back{64, 8, 4};
    back.Prepare(20);
    back.JumpFrames(-40);
    back.Prepare(20);
    // Frames carried once agree exactly. Frames carried twice: the walk finds
    // one of the two packets that carry them.
    for (uint64_t frame = 0; frame < back.nextFrame + 8; ++frame) {
        for (uint64_t hint : {uint64_t{0}, uint64_t{19}, uint64_t{20}, uint64_t{39}}) {
            PacketSlotSnapshot scanned{};
            PacketSlotSnapshot walked{};
            const bool scanFound = back.timeline.SnapshotSlotForAudioFrame(frame, scanned);
            const bool walkFound = back.timeline.SnapshotSlotForAudioFrameNear(frame, hint, walked);
            ASSERT_EQ(walkFound, scanFound) << "frame " << frame << " hint " << hint;
            if (walkFound) {
                EXPECT_GE(frame, walked.firstAudioFrame);
                EXPECT_LT(frame, walked.firstAudioFrame + walked.framesInPacket);
            }
        }
    }
}

// A stream's first packets, as the fill meets them while CoreAudio already
// writes: the ring starts at a packet index with nothing before it, and the
// first DATA packet carries a frame above CoreAudio's first writes.
struct StartingRing : Ring {
    static constexpr uint64_t kFirstPacket = 300;
    static constexpr uint64_t kFirstFrame = 5000;
    StartingRing(uint32_t slotCount, uint32_t framesPerPacketIn, uint32_t dbsIn)
        : Ring(slotCount, framesPerPacketIn, dbsIn) {
        nextPacket = kFirstPacket;
        nextFrame = kFirstFrame;
    }
};

TEST(AmdtpPacketTimelineWalkTests, AFrameBeforeTheFirstPacketMissesAtTheRingsBeginning) {
    StartingRing ring{512, 8, 4};
    ring.Prepare(100);  // packets 300..399, never lapped: 0..299 were never prepared
    for (uint64_t frame = StartingRing::kFirstFrame - 200; frame < ring.nextFrame + 8; ++frame) {
        for (uint64_t hint : {uint64_t{300}, uint64_t{301}, uint64_t{350}, uint64_t{399},
                              uint64_t{400}, uint64_t{430}, uint64_t{250}, uint64_t{0}}) {
            ExpectSameAsScan(ring.timeline, frame, hint);
        }
    }
    // The last prepared packet (399) is NO-DATA and the next is not prepared:
    // the walk still turns back for a frame before it.
    PacketSlotSnapshot snap{};
    ASSERT_TRUE(ring.timeline.SnapshotSlotForAudioFrameNear(ring.nextFrame - 1, 399, snap));
    EXPECT_EQ(snap.packetIndex, 398U);
}

TEST(AmdtpPacketTimelineWalkTests, AMissSaysWhereTheFramesResume) {
    PacketSlotSnapshot snap{};
    uint64_t resume = 99;

    // Before the stream's first packet: its first frame, from any hint in the ring.
    StartingRing ring{512, 8, 4};
    ring.Prepare(100);
    for (uint64_t hint : {uint64_t{300}, uint64_t{350}, uint64_t{399}, uint64_t{430}}) {
        EXPECT_FALSE(ring.timeline.SnapshotSlotForAudioFrameNear(4000, hint, snap, resume));
        EXPECT_EQ(resume, StartingRing::kFirstFrame) << "hint " << hint;
        EXPECT_FALSE(ring.timeline.SnapshotSlotForAudioFrameNear(4999, hint, snap, resume));
        EXPECT_EQ(resume, StartingRing::kFirstFrame) << "hint " << hint;
    }
    // Past the exposed end no packet carries a later frame.
    EXPECT_FALSE(ring.timeline.SnapshotSlotForAudioFrameNear(ring.nextFrame + 3, 399, snap, resume));
    EXPECT_EQ(resume, 0U);
    // A hit leaves nothing to resume.
    EXPECT_TRUE(ring.timeline.SnapshotSlotForAudioFrameNear(5001, 350, snap, resume));
    EXPECT_EQ(resume, 0U);

    // A ring that starts at packet 0, and one that has lapped: the oldest
    // DATA packet's first frame.
    Ring fromZero{64, 8, 4};
    fromZero.nextFrame = 1000;
    fromZero.Prepare(40);
    EXPECT_FALSE(fromZero.timeline.SnapshotSlotForAudioFrameNear(10, 20, snap, resume));
    EXPECT_EQ(resume, 1000U);
    Ring lapped{64, 8, 4};
    lapped.Prepare(200);  // packets 136..199 are live; 136 carries frame 816
    EXPECT_FALSE(lapped.timeline.SnapshotSlotForAudioFrameNear(10, 190, snap, resume));
    EXPECT_EQ(resume, 816U);

    // Frames a cursor jump left out: the first frame after the jump, from
    // either side.
    Ring jump{64, 8, 4};
    jump.Prepare(20);  // frames 0..119
    jump.JumpFrames(100);
    jump.Prepare(20);  // from frame 220
    for (uint64_t hint : {uint64_t{5}, uint64_t{19}, uint64_t{20}, uint64_t{39}}) {
        EXPECT_FALSE(jump.timeline.SnapshotSlotForAudioFrameNear(150, hint, snap, resume));
        EXPECT_EQ(resume, 220U) << "hint " << hint;
    }
}

// --- The writers against the per-frame scan they replaced -------------------

[[nodiscard]] int32_t Float32ToMotuSample(float sample) {
    return static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(sample))
                                << 8);
}

struct ReferenceCounters {
    uint64_t visited{0}, written{0}, without{0}, outside{0}, truncated{0}, missed{0};
};

// MotuPayloadWriter::WriteFloat32Interleaved as it was before the walk: one
// full scan per frame.
void ReferenceMotuWrite(const AmdtpPacketTimeline& timeline,
                        const ASFW::Encoding::Motu::MotuPayloadStreamConfig& config,
                        const HostAudioBufferView& host, uint64_t firstWritable,
                        ReferenceCounters& counters) {
    using namespace ASFW::Encoding::Motu;
    const MotuPortMap ports = EffectivePortMap(config.ports, config.pcmChunks);
    for (uint32_t i = 0; i < host.frameCount; ++i) {
        const uint64_t frame = host.firstFrame + i;
        ++counters.visited;
        PacketSlotSnapshot snap{};
        if (!timeline.SnapshotSlotForAudioFrame(frame, snap)) {
            if (frame >= timeline.ExposedFrameEnd()) {
                ++counters.without;
            } else {
                ++counters.outside;
            }
            continue;
        }
        if (firstWritable != 0 && static_cast<int64_t>(snap.packetIndex - firstWritable) < 0) {
            ++counters.missed;
            continue;
        }
        const uint64_t sourceFrame = host.frameCapacity != 0 ? frame % host.frameCapacity : i;
        const float* source = host.interleavedFloat32 + sourceFrame * host.channels;
        const uint32_t frameInPacket = static_cast<uint32_t>(frame - snap.firstAudioFrame);
        const uint32_t blockBytes = snap.dbs * 4U;
        uint8_t* block = snap.packetBytes + kCipHeaderBytes + frameInPacket * blockBytes;
        if (config.pcmByteOffset + config.pcmChunks * kBytesPerChunk > blockBytes) {
            ++counters.truncated;
            continue;
        }
        for (uint32_t hostCh = 0; hostCh < config.pcmChunks; ++hostCh) {
            const uint32_t chunk = ChunkForHostChannel(ports, hostCh);
            const uint32_t srcCh = config.sourceChannelOffset + hostCh;
            const float sample = srcCh < host.channels ? source[srcCh] : 0.0f;
            WritePcmSample(std::span<uint8_t>(block + config.pcmByteOffset + chunk * kBytesPerChunk,
                                              kBytesPerChunk),
                           Float32ToMotuSample(sample));
        }
        ++counters.written;
    }
}

// The calls the fill makes, plus the irregular ones: a rewind (CoreAudio
// rewrote frames already copied), a skip forward, a frontier that passes the
// call's first packets, and calls that reach past the exposed end.
struct FillCall {
    uint64_t firstFrame;
    uint32_t frameCount;
    uint64_t firstWritable;
};

std::vector<FillCall> MakeFillCalls(const Ring& ring, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<FillCall> calls;
    const uint64_t oldestLive = (ring.nextPacket - ring.slots.size()) / 4 * 3 * ring.framesPerPacket;
    uint64_t frame = oldestLive + 64;
    while (frame + 600 < ring.nextFrame + 200) {
        uint32_t count = std::uniform_int_distribution<uint32_t>(1, 520)(rng);
        const uint32_t kind = std::uniform_int_distribution<uint32_t>(0, 9)(rng);
        uint64_t first = frame;
        if (kind == 0 && first > 300) {
            first -= std::uniform_int_distribution<uint32_t>(1, 300)(rng);  // rewind
        } else if (kind == 1) {
            first += std::uniform_int_distribution<uint32_t>(1, 200)(rng);  // skip
        }
        // The frontier: none, well before the call, or inside it.
        uint64_t firstWritable = 0;
        const uint64_t callPacket = first / (3 * ring.framesPerPacket) * 4;
        if (kind >= 3 && kind <= 5) {
            firstWritable = callPacket > 40 ? callPacket - 40 : 1;
        } else if (kind >= 6) {
            firstWritable = callPacket + std::uniform_int_distribution<uint32_t>(0, 30)(rng);
        }
        calls.push_back({first, count, firstWritable});
        frame = first + count;
    }
    // And one across the exposed end.
    calls.push_back({ring.nextFrame - 100, 300, 0});
    return calls;
}

std::vector<float> MakeHostRing(uint32_t frames, uint32_t channels) {
    std::vector<float> host(static_cast<size_t>(frames) * channels);
    for (size_t i = 0; i < host.size(); ++i) {
        host[i] = static_cast<float>(static_cast<int32_t>(i % 2001) - 1000) / 1024.0f;
    }
    return host;
}

TEST(AmdtpPacketTimelineWalkTests, MotuWriterMatchesThePerFrameScan) {
    using namespace ASFW::Encoding::Motu;
    // The 828mkII at 96 kHz: 16 frames per DATA packet, 14 playback chunks.
    constexpr uint32_t kSlots = 1512;
    constexpr uint32_t kChunks = 14;
    constexpr uint32_t kDbs = DataBlockQuadlets(kChunks);
    constexpr uint32_t kHostFrames = 24576;
    constexpr uint32_t kHostChannels = 16;
    for (uint32_t seed : {1u, 2u, 3u}) {
        Ring actualRing{kSlots, 16, kDbs};
        Ring referenceRing{kSlots, 16, kDbs};
        actualRing.Prepare(3000);
        referenceRing.Prepare(3000);
        const MotuPayloadStreamConfig config{.pcmChunks = kChunks,
                                             .sourceChannelOffset = 0,
                                             .ports = kV2Playback};
        MotuPayloadWriter writer{};
        writer.Configure(config);
        writer.BindTimeline(&actualRing.timeline);
        const std::vector<float> host = MakeHostRing(kHostFrames, kHostChannels);
        ReferenceCounters reference{};
        for (const FillCall& call : MakeFillCalls(actualRing, seed)) {
            HostAudioBufferView view{};
            view.interleavedFloat32 = host.data();
            view.firstFrame = call.firstFrame;
            view.frameCount = call.frameCount;
            view.frameCapacity = kHostFrames;
            view.channels = kHostChannels;
            writer.WriteFloat32Interleaved(view, call.firstWritable);
            ReferenceMotuWrite(referenceRing.timeline, config, view, call.firstWritable, reference);
        }
        EXPECT_EQ(actualRing.bytes, referenceRing.bytes) << "seed " << seed;
        const auto& counters = writer.Counters();
        EXPECT_EQ(counters.framesVisited.load(), reference.visited);
        EXPECT_EQ(counters.framesWritten.load(), reference.written);
        EXPECT_EQ(counters.framesWithoutPacket.load(), reference.without);
        EXPECT_EQ(counters.framesOutsidePacket.load(), reference.outside);
        EXPECT_EQ(counters.framesTruncated.load(), reference.truncated);
        EXPECT_EQ(counters.framesMissedFinality.load(), reference.missed);
        EXPECT_GT(reference.written, 0U);
        EXPECT_GT(reference.missed, 0U);
        EXPECT_GT(reference.without, 0U);
    }
}

TEST(AmdtpPacketTimelineWalkTests, MotuWriterReportsItsFinalityMargin) {
    using namespace ASFW::Encoding::Motu;
    Ring ring{64, 8, DataBlockQuadlets(2)};
    ring.Prepare(40);
    MotuPayloadWriter writer{};
    writer.Configure(MotuPayloadStreamConfig{.pcmChunks = 2, .sourceChannelOffset = 0});
    writer.BindTimeline(&ring.timeline);
    EXPECT_EQ(writer.TakeMinFinalityMarginPackets(), INT64_MAX);
    const std::vector<float> host = MakeHostRing(256, 2);
    HostAudioBufferView view{};
    view.interleavedFloat32 = host.data();
    view.firstFrame = 48;  // packet 8's first frame
    view.frameCount = 16;
    view.frameCapacity = 256;
    view.channels = 2;
    writer.WriteFloat32Interleaved(view, 5);
    EXPECT_EQ(writer.TakeMinFinalityMarginPackets(), 3);
    EXPECT_EQ(writer.TakeMinFinalityMarginPackets(), INT64_MAX);
}

// AmdtpPayloadWriter::WriteFloat32Interleaved before the walk, for the
// default policy (identity map, AM824 MBLA): one scan per packet.
void ReferenceAmdtpWrite(const AmdtpPacketTimeline& timeline, const AmdtpStreamConfig& config,
                         const AmdtpTxPolicy& policy, const HostAudioBufferView& host,
                         uint64_t firstWritable, ReferenceCounters& counters) {
    for (uint32_t i = 0; i < host.frameCount; ++i) {
        const uint64_t frame = host.firstFrame + i;
        ++counters.visited;
        PacketSlotSnapshot snap{};
        if (!timeline.SnapshotSlotForAudioFrame(frame, snap)) {
            if (frame >= timeline.ExposedFrameEnd()) {
                ++counters.without;
            } else {
                ++counters.outside;
            }
            continue;
        }
        if (firstWritable != 0 && static_cast<int64_t>(snap.packetIndex - firstWritable) < 0) {
            ++counters.missed;
            continue;
        }
        const uint64_t sourceFrame = host.frameCapacity != 0 ? frame % host.frameCapacity : i;
        const float* source = host.interleavedFloat32 + sourceFrame * host.channels;
        const uint32_t frameInPacket = static_cast<uint32_t>(frame - snap.firstAudioFrame);
        uint8_t* dest = snap.packetBytes + kCipHeaderBytes + frameInPacket * snap.dbs * 4U;
        const uint32_t pcmSlots = config.pcmChannels < snap.dbs ? config.pcmChannels : snap.dbs;
        for (uint32_t ch = 0; ch < pcmSlots; ++ch) {
            const uint32_t srcCh = config.sourceChannelOffset + ch;
            const float sample = srcCh < host.channels ? source[srcCh] : 0.0f;
            const uint32_t word = PcmSlotCodec::EncodeFloat32(sample, policy.hostToDevicePcmEncoding);
            uint8_t* at = dest + ch * 4U;
            at[0] = static_cast<uint8_t>(word >> 24);
            at[1] = static_cast<uint8_t>(word >> 16);
            at[2] = static_cast<uint8_t>(word >> 8);
            at[3] = static_cast<uint8_t>(word);
        }
        ++counters.written;
    }
}

TEST(AmdtpPacketTimelineWalkTests, AmdtpWriterMatchesThePerFrameScan) {
    constexpr uint32_t kSlots = 1512;
    constexpr uint32_t kChannels = 10;
    constexpr uint32_t kHostFrames = 24576;
    for (uint32_t seed : {4u, 5u}) {
        Ring actualRing{kSlots, 8, kChannels};
        Ring referenceRing{kSlots, 8, kChannels};
        actualRing.Prepare(3000);
        referenceRing.Prepare(3000);
        AmdtpStreamConfig config{};
        config.dbs = kChannels;
        config.pcmChannels = kChannels;
        const AmdtpTxPolicy policy{};
        AmdtpPayloadWriter writer{};
        writer.Configure(config, policy);
        writer.BindTimeline(&actualRing.timeline);
        const std::vector<float> host = MakeHostRing(kHostFrames, kChannels);
        ReferenceCounters reference{};
        for (const FillCall& call : MakeFillCalls(actualRing, seed)) {
            HostAudioBufferView view{};
            view.interleavedFloat32 = host.data();
            view.firstFrame = call.firstFrame;
            view.frameCount = call.frameCount;
            view.frameCapacity = kHostFrames;
            view.channels = kChannels;
            writer.WriteFloat32Interleaved(view, call.firstWritable);
            ReferenceAmdtpWrite(referenceRing.timeline, config, policy, view, call.firstWritable,
                                reference);
        }
        EXPECT_EQ(actualRing.bytes, referenceRing.bytes) << "seed " << seed;
        const auto& counters = writer.Counters();
        EXPECT_EQ(counters.framesVisited.load(), reference.visited);
        EXPECT_EQ(counters.framesWritten.load(), reference.written);
        EXPECT_EQ(counters.framesWithoutPacket.load(), reference.without);
        EXPECT_EQ(counters.framesOutsidePacket.load(), reference.outside);
        EXPECT_EQ(counters.framesMissedFinality.load(), reference.missed);
        EXPECT_GT(reference.written, 0U);
    }
}

} // namespace
