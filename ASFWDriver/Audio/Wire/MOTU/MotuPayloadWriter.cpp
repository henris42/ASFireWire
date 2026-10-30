// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuPayloadWriter.cpp - see MotuPayloadWriter.hpp.

#include "MotuPayloadWriter.hpp"

#include "../AMDTP/PcmSlotCodec.hpp"
#include "../../../Logging/Logging.hpp"

#include <cstdio>

namespace ASFW::Encoding::Motu {

namespace {

using ASFW::Protocols::Audio::AMDTP::PacketSlotSnapshot;
using ASFW::Protocols::Audio::AMDTP::PcmSlotCodec;

/// CIP header precedes the first data block, same as every other 61883-6 stream.
constexpr uint32_t kCipHeaderBytes = 8;

/// MotuBlockCodec::WritePcmSample takes the sample in the 24 significant MSBs of an
/// int32 (amdtp-motu.c:150-156), while PcmSlotCodec::Float32ToSigned24 returns it in the
/// low 24 bits. Shift bridges the two conventions.
[[nodiscard]] inline int32_t Float32ToMotuSample(float sample) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(sample))
                                << 8);
}

} // namespace

void MotuPayloadWriter::Configure(const MotuPayloadStreamConfig& streamConfig) noexcept {
    dumpedPackets_ = 0;
    dumpedAudiblePackets_ = 0;
    streamConfig_ = streamConfig;
    packetHint_.store(0, std::memory_order_relaxed);
}

void MotuPayloadWriter::BindTimeline(
    Protocols::Audio::AMDTP::AmdtpPacketTimeline* timeline) noexcept {
    timeline_ = timeline;
    packetHint_.store(0, std::memory_order_relaxed);
}

const MotuPayloadWriterCounters& MotuPayloadWriter::Counters() const noexcept {
    return counters_;
}

void MotuPayloadWriter::SetPcmMuted(bool muted) noexcept {
    muted_.store(muted, std::memory_order_release);
}

bool MotuPayloadWriter::IsPcmMuted() const noexcept {
    return muted_.load(std::memory_order_acquire);
}

void MotuPayloadWriter::WriteFloat32Interleaved(
    const Protocols::Audio::AMDTP::HostAudioBufferView& hostBuffer,
    uint64_t firstWritablePacket) noexcept {

    if (timeline_ == nullptr || hostBuffer.interleavedFloat32 == nullptr ||
        hostBuffer.channels == 0 || hostBuffer.frameCount == 0 ||
        streamConfig_.pcmChunks == 0) {
        return; // invalid view or unconfigured geometry: nothing visited, nothing counted
    }

    uint64_t written = 0;
    uint64_t withoutPacket = 0;
    uint64_t outsidePacket = 0;
    uint64_t truncated = 0;
    uint64_t missedFinality = 0;
    const MotuPortMap ports = EffectivePortMap(streamConfig_.ports, streamConfig_.pcmChunks);
    // One decision per call: a call never mixes muted and audible frames.
    const bool muted = muted_.load(std::memory_order_acquire);
    uint32_t peakQ24 = 0;
    // The peak feeds only the [TxContent] debug heartbeat, the packet dumps
    // only trace: neither costs anything below its level.
    const bool meter = ASFW_LOG_ENABLED(DirectAudio, Debug);
    const bool dump = ASFW_LOG_ENABLED(DirectAudio, Trace);
    // A short block means the configured chunk count disagrees with the packet
    // geometry; writing anyway would corrupt the next block's SPH.
    const uint32_t requiredBytes =
        streamConfig_.pcmByteOffset + streamConfig_.pcmChunks * kBytesPerChunk;
    const uint32_t srcOffset = streamConfig_.sourceChannelOffset;

    // Packet by packet: one timeline lookup per packet, starting where the
    // previous call stopped (or at the finality frontier), then every frame
    // of the call that packet carries.
    const uint64_t endFrame = hostBuffer.firstFrame + hostBuffer.frameCount;
    uint64_t hint = packetHint_.load(std::memory_order_relaxed);
    bool haveHint = hint != 0;
    if (haveHint) {
        --hint;
    } else if (firstWritablePacket != 0) {
        hint = firstWritablePacket;
        haveHint = true;
    }
    uint64_t frame = hostBuffer.firstFrame;
    while (frame < endFrame) {
        PacketSlotSnapshot snap{};
        const bool found = haveHint
                               ? timeline_->SnapshotSlotForAudioFrameNear(frame, hint, snap)
                               : timeline_->SnapshotSlotForAudioFrame(frame, snap);
        if (!found) {
            // Past the exposed end means "no packet owns this yet" (normal lead);
            // behind it means the slot was retired or re-used under us.
            if (frame >= timeline_->ExposedFrameEnd()) {
                ++withoutPacket;
            } else {
                ++outsidePacket;
            }
            ++frame;
            continue;
        }
        // The snapshot's range check guarantees firstAudioFrame <= frame <
        // firstAudioFrame + framesInPacket, so the run is not empty.
        const uint64_t packetEnd = snap.firstAudioFrame + snap.framesInPacket;
        const uint64_t runEnd = packetEnd < endFrame ? packetEnd : endFrame;
        const uint64_t runFrames = runEnd - frame;
        hint = snap.packetIndex + 1;
        haveHint = true;

        if (firstWritablePacket != 0 &&
            static_cast<int64_t>(snap.packetIndex - firstWritablePacket) < 0) {
            missedFinality += runFrames;
            frame = runEnd;
            continue;
        }

        // dbs is the data block size in quadlets, which is exactly what
        // DataBlockQuadlets() produces for this chunk count -- one SPH quadlet plus the
        // message and PCM chunks, padded to quadlet alignment.
        const uint32_t blockBytes = snap.dbs * 4U;
        if (requiredBytes > blockBytes) {
            truncated += runFrames;
            frame = runEnd;
            continue;
        }

        uint64_t sourceFrame = hostBuffer.frameCapacity != 0
                                   ? frame % hostBuffer.frameCapacity
                                   : frame - hostBuffer.firstFrame;
        for (; frame < runEnd; ++frame) {
            const float* const source =
                hostBuffer.interleavedFloat32 + sourceFrame * hostBuffer.channels;
            if (++sourceFrame == hostBuffer.frameCapacity) {
                sourceFrame = 0;
            }
            const uint32_t frameInPacket = static_cast<uint32_t>(frame - snap.firstAudioFrame);
            uint8_t* const block = snap.packetBytes + kCipHeaderBytes + frameInPacket * blockBytes;

            // Walk host channels rather than chunks: the port map is a permutation of the
            // chunk range, so every chunk is still written exactly once.
            for (uint32_t hostCh = 0; hostCh < streamConfig_.pcmChunks; ++hostCh) {
                const uint32_t chunk = ChunkForHostChannel(ports, hostCh);
                const uint32_t srcCh = srcOffset + hostCh;
                // Chunks beyond the host buffer encode PCM zero -- MOTU's fixed chunk count
                // always covers every physical port, including ones CoreAudio is not
                // driving, and those must carry silence rather than stale bytes.
                const float sample = (!muted && srcCh < hostBuffer.channels) ? source[srcCh] : 0.0f;
                const int32_t encoded = Float32ToMotuSample(sample);
                // The 24 significant bits sit in the high bytes; their magnitude is the meter.
                if (meter) {
                    const uint32_t magnitude =
                        static_cast<uint32_t>(encoded < 0 ? -(encoded >> 8) : (encoded >> 8));
                    if (magnitude > peakQ24) {
                        peakQ24 = magnitude;
                    }
                }
                WritePcmSample(
                    std::span<uint8_t>(block + streamConfig_.pcmByteOffset + chunk * kBytesPerChunk,
                                       kBytesPerChunk),
                    encoded);
            }
        }
        written += runFrames;
        if (dump && runEnd == packetEnd &&
            (dumpedPackets_ < kDumpPackets || (!muted && dumpedAudiblePackets_ < kDumpPackets))) {
            DumpFilledPacket(snap, muted);
        }
    }
    if (haveHint) {
        packetHint_.store(hint + 1, std::memory_order_relaxed);
    }

    if (peakQ24 != 0) {
        uint32_t current = counters_.intervalPeakWrittenQ24.load(std::memory_order_relaxed);
        while (peakQ24 > current &&
               !counters_.intervalPeakWrittenQ24.compare_exchange_weak(current, peakQ24,
                                                                       std::memory_order_relaxed)) {
        }
    }
    counters_.framesVisited.fetch_add(hostBuffer.frameCount, std::memory_order_relaxed);
    counters_.framesWritten.fetch_add(written, std::memory_order_relaxed);
    counters_.framesWithoutPacket.fetch_add(withoutPacket, std::memory_order_relaxed);
    counters_.framesOutsidePacket.fetch_add(outsidePacket, std::memory_order_relaxed);
    counters_.framesTruncated.fetch_add(truncated, std::memory_order_relaxed);
    counters_.framesMissedFinality.fetch_add(missedFinality, std::memory_order_relaxed);
    if (muted) {
        counters_.framesMuted.fetch_add(written, std::memory_order_relaxed);
    }
}

void MotuPayloadWriter::DumpFilledPacket(const PacketSlotSnapshot& snap, bool muted) noexcept {
    if (snap.packetBytes == nullptr || snap.packetSizeBytes < kCipHeaderBytes || snap.dbs == 0) {
        return;
    }
    ++dumpedPackets_;
    if (!muted) {
        ++dumpedAudiblePackets_;
    }
    const auto be32 = [](const uint8_t* b) {
        return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
               (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
    };
    const uint32_t blockBytes = snap.dbs * 4U;

    // Line 1: CIP words and every block's SPH (cycle:offset).
    char sph[8 * 12 + 1] = {};
    size_t used = 0;
    for (uint32_t block = 0; block < snap.framesInPacket && block < 8; ++block) {
        const size_t at = kCipHeaderBytes + static_cast<size_t>(block) * blockBytes;
        if (at + 4 > snap.packetSizeBytes || used + 12 > sizeof(sph)) break;
        const uint32_t value = be32(snap.packetBytes + at);
        used += static_cast<size_t>(std::snprintf(sph + used, sizeof(sph) - used, " %u:%u",
                                                  (value >> 12) & 0x1fffU, value & 0xfffU));
    }
    ASFW_LOG_TRACE(DirectAudio,
                   "[MotuTxDump] pkt=%llu bytes=%u blocks=%u dbs=%u muted=%u cip=%08x %08x sph=%{public}s",
                   static_cast<unsigned long long>(snap.packetIndex), snap.packetSizeBytes,
                   snap.framesInPacket, snap.dbs, muted ? 1U : 0U, be32(snap.packetBytes),
                   be32(snap.packetBytes + 4), sph);

    // Line 2: the first data block, as quadlets.
    char hex[19 * 9 + 1] = {};
    used = 0;
    for (uint32_t q = 0; q < snap.dbs && q < 19; ++q) {
        const size_t at = kCipHeaderBytes + static_cast<size_t>(q) * 4U;
        if (at + 4 > snap.packetSizeBytes || used + 9 > sizeof(hex)) break;
        used += static_cast<size_t>(std::snprintf(hex + used, sizeof(hex) - used, "%08x ",
                                                  be32(snap.packetBytes + at)));
    }
    ASFW_LOG_TRACE(DirectAudio, "[MotuTxDump] pkt=%llu block0=%{public}s",
                   static_cast<unsigned long long>(snap.packetIndex), hex);
}

} // namespace ASFW::Encoding::Motu
