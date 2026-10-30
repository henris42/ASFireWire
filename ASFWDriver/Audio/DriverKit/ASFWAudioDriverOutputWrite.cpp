//
// ASFWAudioDriverOutputWrite.cpp
// ASFWDriver
//
// The CoreAudio output side of TX (documentation/TX_OWNERSHIP.md):
//
// - WriteEnd publishes the client's write end W and the playback-ring range,
//   fills, and wakes the TX producer. The HAL output ring CoreAudio just
//   wrote is the PCM publication.
// - FillTransmitPayloads, run inside WriteEnd on the IO thread (its only
//   owner), copies each written frame once from that ring into the armed
//   packet that carries it, if the packet is still ahead of the hardware.
//   RT-safe: no locks, no allocation; the timeline and the clock pair are
//   read through their seqlocks. A frame is cleared in the ring a short
//   window after it has been copied, and the whole ring at every start
//   (ClearPlaybackRing), so a frame CoreAudio never wrote is silence rather
//   than an older write.
// - FadeTransmitTailToSilence, at StopIO, fades the copied frames still ahead
//   of the hardware to zero and rewrites them, so the stream ends in silence
//   (b155: the 828mkII holds the last sample it received).
//
// Both are called unchanged from the host TX tests.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../Runtime/PlaybackRingRange.hpp"
#include "../../Common/TimingUtils.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#endif

namespace ASFW::Audio::DriverKit {
namespace {

void PublishPlaybackRingWriteEnd(ASFW::Audio::Runtime::AudioGraphBinding& graph,
                                 ASFW::Audio::Runtime::AudioTransportControlBlock& control) noexcept {
    const uint64_t writeStart =
        control.client.outputWriteEndSampleFrame.load(std::memory_order_relaxed);
    const uint64_t writeEnd = control.client.OutputWrittenEndFrame();
    const uint64_t previous =
        control.playbackRingWriteFrame.load(std::memory_order_acquire);
    const uint64_t previousOldest =
        control.playbackRingOldestValidFrame.load(std::memory_order_acquire);
    const uint64_t consumed =
        control.playbackRingReadFrame.load(std::memory_order_acquire);
    const uint32_t capacity = graph.memory.outputFrameCapacity;
    const auto update = ASFW::Audio::Runtime::UpdatePlaybackRingRange(
        previous, previousOldest, writeStart, writeEnd, consumed, capacity);
    if (update.writtenEndFrame == previous) {
        return;
    }

    control.playbackRingOldestValidFrame.store(update.oldestValidFrame,
                                               std::memory_order_relaxed);
    if (update.discontinuity) {
        control.playbackRingDiscontinuityGeneration.fetch_add(1, std::memory_order_relaxed);
        control.discontinuities.fetch_add(1, std::memory_order_relaxed);
    }
    if (update.overrun) {
        control.playbackRingOverruns.fetch_add(1, std::memory_order_relaxed);
    }
    if (update.rewound) {
        control.playbackRingRewinds.fetch_add(1, std::memory_order_relaxed);
    }
    control.playbackRingWriteFrame.store(update.writtenEndFrame, std::memory_order_release);
}

} // namespace

bool HandleOutputWriteEnd(ASFWAudioDriver_IVars& ivars,
                          ASFW::Audio::Runtime::AudioTransportControlBlock& control,
                          uint64_t sampleTime,
                          uint64_t hostTime,
                          uint32_t ioBufferFrameSize) noexcept {
    // CoreAudio may choose a larger span than kHalIoPeriodFrames while
    // remaining within the stream ring; the ring capacity is the hard bound.
    if (ioBufferFrameSize > ivars.runtime.directAudioGraph.memory.outputFrameCapacity) {
        return false;
    }
    control.RecordOutputWrite(sampleTime, ioBufferFrameSize);
    control.client.PublishWriteEnd(sampleTime, hostTime, ioBufferFrameSize);
    PublishPlaybackRingWriteEnd(ivars.runtime.directAudioGraph, control);

    // Fill here, on CoreAudio's IO thread, not on the TX preparation queue: a
    // frame has ~1 ms between this write and its packet's deadline, and queue
    // wakes were measured up to 13.7 ms late on hardware, leaving packets
    // silent (TX_OWNERSHIP.md §1d). This thread is the fill's only owner.
    FillTransmitPayloads(ivars);

    // Wake the producer so packet arming keeps pace with CoreAudio. The
    // coalescing latch keeps this RT callback to at most one outstanding
    // action.
    const uint64_t requestGeneration =
        control.txPreparationRequests.PublishRequest(hostTime);
    if (ivars.device.audioNub && ivars.txPreparationAction &&
        control.txPreparationRequests.TryScheduleWake()) {
        // OSAction-targeted, one-way dispatch goes directly to TxPreparation.
        // Never wait behind a device-control RPC on the nub's Default queue.
        ivars.device.audioNub->TxPreparationReady(ivars.txPreparationAction.get(), requestGeneration);
    }

    control.counters.CountWriteEnd();
    return true;
}

namespace {

[[nodiscard]] uint64_t TxHostNow() noexcept {
#ifdef ASFW_HOST_TEST
    return ASFW::Testing::HostMonotonicNow();
#else
    return mach_absolute_time();
#endif
}

// The packet the hardware is sending now, projected from the last refill:
// completionCursor was recorded with the clock pair of that refill, and IT
// sends one packet per bus cycle, so the packets since then are the elapsed
// cycles. Rounded up, so the fill errs toward treating a packet as taken.
[[nodiscard]] uint64_t ProjectHardwarePacket(const ASFW::Isoch::IsochTxQueueControl& queue,
                                             uint64_t nowHost) noexcept {
    const uint64_t completion = queue.completionCursor.load(std::memory_order_acquire);
    ASFW::Isoch::IsochTxClockPairSample pair{};
    if (!queue.clockPair.TryRead(pair) || pair.hostTimeMid == 0 || nowHost < pair.hostTimeMid) {
        // No usable correlation: assume a whole interrupt group has gone out.
        return completion + ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
    }
    const uint64_t elapsedNanos = ASFW::Timing::hostTicksToNanos(nowHost - pair.hostTimeMid);
    constexpr uint64_t kCycleNanos = 125'000;
    return completion + (elapsedNanos + kCycleNanos - 1) / kCycleNanos;
}

// Zeroes `count` frames of the playback ring from absolute frame `first`,
// across the wrap.
void ClearRingFrames(const ASFW::Audio::Runtime::AudioStreamMemory& memory,
                     uint64_t first,
                     uint64_t count) noexcept {
    const uint32_t capacity = memory.outputFrameCapacity;
    if (memory.outputBase == nullptr || capacity == 0 || memory.outputChannels == 0 ||
        count == 0) {
        return;
    }
    if (count > capacity) {
        first += count - capacity;
        count = capacity;
    }
    float* const base = memory.outputBase;
    const size_t frameBytes = static_cast<size_t>(memory.outputChannels) * sizeof(float);
    const uint64_t start = first % capacity;
    const uint64_t firstRun = count < capacity - start ? count : capacity - start;
    std::memset(base + start * memory.outputChannels, 0,
                static_cast<size_t>(firstRun) * frameBytes);
    if (count > firstRun) {
        std::memset(base, 0, static_cast<size_t>(count - firstRun) * frameBytes);
    }
}

// Copied frames the ring keeps behind the fill, for the end of the stream. A
// frame is copied at most about one IO buffer before its packet leaves (1024
// frames is the HAL's largest), so 2048 covers every frame still ahead of the
// hardware. At most a quarter of the ring, so a frame is cleared long before
// CoreAudio writes its slot again.
[[nodiscard]] uint64_t KeptCopiedFrames(uint32_t capacity) noexcept {
    constexpr uint64_t kKeptCopiedFrames = 2048;
    return std::min<uint64_t>(kKeptCopiedFrames, capacity / 4);
}

// Clears the copied frames in [clearedEnd, filledEnd - kept) and advances
// clearedEnd. Never reaches back more than a ring before writtenEnd: those
// slots hold the kept frames and the frames written but not yet copied.
void ClearCopiedFrames(const ASFW::Audio::Runtime::AudioStreamMemory& memory,
                       uint64_t& clearedEnd,
                       uint64_t filledEnd,
                       uint64_t writtenEnd) noexcept {
    const uint32_t capacity = memory.outputFrameCapacity;
    const uint64_t kept = KeptCopiedFrames(capacity);
    if (filledEnd <= kept) {
        return;
    }
    const uint64_t limit = filledEnd - kept;
    const uint64_t floor = writtenEnd > capacity ? writtenEnd - capacity : 0;
    const uint64_t first = std::max(clearedEnd, floor);
    if (limit > first) {
        ClearRingFrames(memory, first, limit - first);
    }
    clearedEnd = std::max(clearedEnd, limit);
}

} // namespace

void ClearPlaybackRing(ASFWAudioDriver_IVars& ivars) noexcept {
    const auto& memory = ivars.runtime.directAudioGraph.memory;
    ClearRingFrames(memory, 0, memory.outputFrameCapacity);
}

void FillTransmitPayloads(ASFWAudioDriver_IVars& ivars) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    const auto* queue = ivars.runtime.txSlotProvider.queueControl;
    const auto& memory = ivars.runtime.directAudioGraph.memory;
    if (control == nullptr || queue == nullptr || memory.outputBase == nullptr ||
        memory.outputChannels == 0 || memory.outputFrameCapacity == 0) {
        return;
    }

    // What CoreAudio has written and the ring still holds.
    const uint64_t writtenEnd = control->playbackRingWriteFrame.load(std::memory_order_acquire);
    const uint64_t oldestValid =
        control->playbackRingOldestValidFrame.load(std::memory_order_acquire);
    uint64_t& filled = ivars.runtime.txFilledFrameEnd;
    uint64_t& cleared = ivars.runtime.txClearedFrameEnd;
    const uint64_t writeStart =
        control->client.outputWriteEndSampleFrame.load(std::memory_order_acquire);
    if (filled == 0) {
        // Frames before CoreAudio's first write were never written: there is
        // nothing to fill, and they must not count as missed deadlines. The
        // start cleared the ring, so nothing below it needs clearing again.
        filled = writeStart;
        cleared = writeStart;
    } else if (writeStart < filled) {
        // CoreAudio wrote frames this fill has already copied: an overlapping
        // write, or a written end that had run ahead and was just followed
        // back (PlaybackRingRange rewound). Copy them again, so every packet
        // still ahead of the hardware carries CoreAudio's latest write.
        filled = writeStart;
    }
    // Frames copied again are cleared again once the fill has passed them.
    cleared = std::min(cleared, filled);
    if (filled < oldestValid) {
        filled = oldestValid;  // frames the ring no longer holds stay silent
    }
    // Only frames whose packets exist yet; the rest wait for a later pass.
    const uint64_t exposedEnd = ivars.runtime.txStreamEngine.Timeline().ExposedFrameEnd();
    const uint64_t end = writtenEnd < exposedEnd ? writtenEnd : exposedEnd;
    if (end <= filled) {
        return;
    }

    const uint64_t firstWritable =
        ProjectHardwarePacket(*queue, TxHostNow()) +
        ASFW::IsochTransport::AudioTimingGeometry::kTxFillFinalityGuardPackets;

    const uint64_t frames = end - filled;
    ASFW::Protocols::Audio::AMDTP::HostAudioBufferView hostBuffer{};
    hostBuffer.interleavedFloat32 = memory.outputBase;
    hostBuffer.firstFrame = filled;
    hostBuffer.frameCount = frames > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(frames);
    hostBuffer.frameCapacity = memory.outputFrameCapacity;
    hostBuffer.channels = memory.outputChannels;

    const uint64_t fillStarted = TxHostNow();
    ivars.runtime.txStreamEngine.FillFromHostOutput(hostBuffer, firstWritable);
    // The secondary stream carries host channels [16, 32) of the same frames
    // (its writer's sourceChannelOffset), so both streams take one decision.
    if (ivars.runtime.txSecondaryActive) {
        ivars.runtime.txStreamEngineSecondary.FillFromHostOutput(hostBuffer, firstWritable);
    }

    const uint64_t fillDuration = TxHostNow() - fillStarted;
    auto& maxDuration = ivars.runtime.txFillMaxDurationTicks;
    uint64_t observed = maxDuration.load(std::memory_order_relaxed);
    while (fillDuration > observed &&
           !maxDuration.compare_exchange_weak(observed, fillDuration,
                                             std::memory_order_relaxed)) {}
    // Clear what was copied a short window ago. The ring keeps a frame until
    // CoreAudio writes that slot again a ring later; a slot it skips, or a
    // frame it reported without writing, must then read as silence, not as
    // this one. The newest copied frames stay, for the end of the stream.
    filled += hostBuffer.frameCount;
    ClearCopiedFrames(memory, cleared, filled, writtenEnd);
    control->playbackRingReadFrame.store(filled, std::memory_order_release);
}

namespace {

// The stop's fade: 2 ms (rate / 500), at least 16 frames. Shorter when less
// of the stream is still ahead of the hardware.
constexpr uint32_t kStopFadeRateDivisor = 500;
constexpr uint64_t kMinStopFadeFrames = 16;
// Packets past the finality frontier where the stop looks for its first
// frame, so the frontier cannot pass that frame before the rewrite runs.
constexpr uint64_t kStopMarginPackets = 4;

[[nodiscard]] uint64_t MissedFinalityFrames(const ASFWAudioDriver_IVars& ivars) noexcept {
    return ivars.runtime.txStreamEngine.PayloadWriterCounters().framesMissedFinality.load(
               std::memory_order_relaxed) +
           ivars.runtime.motuPayloadWriter.Counters().framesMissedFinality.load(
               std::memory_order_relaxed);
}

} // namespace

uint64_t ProjectedTransmitPacket(const ASFWAudioDriver_IVars& ivars) noexcept {
    const auto* queue = ivars.runtime.txSlotProvider.queueControl;
    return queue != nullptr ? ProjectHardwarePacket(*queue, TxHostNow()) : 0;
}

TransmitTailSilence FadeTransmitTailToSilence(ASFWAudioDriver_IVars& ivars) noexcept {
    TransmitTailSilence result{};
    const auto* queue = ivars.runtime.txSlotProvider.queueControl;
    const auto& memory = ivars.runtime.directAudioGraph.memory;
    const uint64_t copiedEnd = ivars.runtime.txFilledFrameEnd;
    const uint32_t channels = memory.outputChannels;
    if (queue == nullptr || memory.outputBase == nullptr || channels == 0 ||
        channels > ASFW::Audio::Runtime::kMaxHostPcmChannels ||
        memory.outputFrameCapacity == 0 || copiedEnd == 0) {
        return result;  // nothing was ever copied into a packet
    }
    auto& engine = ivars.runtime.txStreamEngine;
    const auto& timeline = engine.Timeline();

    // The last packet that carries a copied frame. Its slot retired means the
    // stream's end went out long ago. Every lookup here walks the timeline
    // from a nearby packet (SnapshotSlotForAudioFrameNear).
    const uint64_t projected = ProjectHardwarePacket(*queue, TxHostNow());
    ASFW::Protocols::Audio::AMDTP::PacketSlotSnapshot last{};
    if (!timeline.SnapshotSlotForAudioFrameNear(copiedEnd - 1, projected, last)) {
        return result;
    }
    result.audible = true;
    result.lastAudiblePacket = last.packetIndex;

    // The fade's first frame: the first kept frame whose packet the writer can
    // still reach, at the latest the frame after the last copied one. Any
    // later and the stream would drop to silence before the fade began.
    const uint32_t capacity = memory.outputFrameCapacity;
    const uint64_t kept = KeptCopiedFrames(capacity);
    const uint64_t reachable = ProjectHardwarePacket(*queue, TxHostNow()) +
                               ASFW::IsochTransport::AudioTimingGeometry::kTxFillFinalityGuardPackets +
                               kStopMarginPackets;
    uint64_t fadeStart = UINT64_MAX;
    uint64_t frame = std::max(copiedEnd > kept ? copiedEnd - kept : 0,
                              ivars.runtime.txClearedFrameEnd);
    uint64_t hint = reachable;
    while (frame <= copiedEnd) {
        ASFW::Protocols::Audio::AMDTP::PacketSlotSnapshot snap{};
        if (!timeline.SnapshotSlotForAudioFrameNear(frame, hint, snap) ||
            snap.framesInPacket == 0) {
            ++frame;
            continue;
        }
        if (static_cast<int64_t>(snap.packetIndex - reachable) >= 0) {
            fadeStart = frame;
            break;
        }
        frame = snap.firstAudioFrame + snap.framesInPacket;
        hint = snap.packetIndex + 1;
    }
    if (fadeStart == UINT64_MAX) {
        return result;  // CoreAudio's last frame is already on its way out
    }

    // The fade always runs its full length: past CoreAudio's last write it
    // fades that frame's samples, held, so the wave never steps. Copied
    // frames after the fade are zeroed.
    std::array<float, ASFW::Audio::Runtime::kMaxHostPcmChannels> held{};
    const float* const lastWritten = memory.outputBase + ((copiedEnd - 1) % capacity) * channels;
    std::copy(lastWritten, lastWritten + channels, held.begin());
    const uint32_t rate = static_cast<uint32_t>(ivars.device.currentSampleRate);
    const uint64_t fadeFrames = std::max<uint64_t>(rate / kStopFadeRateDivisor, kMinStopFadeFrames);
    const uint64_t fadeEnd = fadeStart + fadeFrames;
    const uint64_t end = std::max(copiedEnd, fadeEnd);
    float peak = 0.0f;
    for (uint64_t at = fadeStart; at < end; ++at) {
        float* const samples = memory.outputBase + (at % capacity) * channels;
        const float gain = at < fadeEnd ? static_cast<float>(fadeEnd - 1 - at) /
                                              static_cast<float>(fadeFrames)
                                        : 0.0f;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            const float source = at < copiedEnd ? samples[ch] : held[ch];
            peak = std::max(peak, source < 0.0f ? -source : source);
            samples[ch] = source * gain;
        }
    }

    // Rewrite the tail into its packets through the stream's own writer.
    ASFW::Protocols::Audio::AMDTP::HostAudioBufferView hostBuffer{};
    hostBuffer.interleavedFloat32 = memory.outputBase;
    hostBuffer.firstFrame = fadeStart;
    hostBuffer.frameCount = static_cast<uint32_t>(end - fadeStart);
    hostBuffer.frameCapacity = capacity;
    hostBuffer.channels = channels;
    const uint64_t missedBefore = MissedFinalityFrames(ivars);
    const uint64_t firstWritable =
        ProjectHardwarePacket(*queue, TxHostNow()) +
        ASFW::IsochTransport::AudioTimingGeometry::kTxFillFinalityGuardPackets;
    engine.FillFromHostOutput(hostBuffer, firstWritable);
    if (ivars.runtime.txSecondaryActive) {
        ivars.runtime.txStreamEngineSecondary.FillFromHostOutput(hostBuffer, firstWritable);
    }
    ClearRingFrames(memory, fadeStart, end - fadeStart);

    result.fadedFrames = fadeFrames;
    result.heldFrames = fadeEnd > copiedEnd ? fadeEnd - std::max(copiedEnd, fadeStart) : 0;
    result.silencedFrames = copiedEnd > fadeEnd ? copiedEnd - fadeEnd : 0;
    result.missedFrames = std::min(MissedFinalityFrames(ivars) - missedBefore, end - fadeStart);
    result.peakQ24 = static_cast<uint32_t>(std::min(peak, 1.0f) * 8388607.0f);
    // The last frame that can still be nonzero on the wire: the fade's last
    // nonzero frame, or the last copied frame whose packet the rewrite missed.
    const uint64_t lastFaded = fadeEnd - 2;  // the fade's last frame is zero
    uint64_t lastAudible = lastFaded;
    if (result.missedFrames != 0) {
        lastAudible = std::max(lastAudible, std::min(fadeStart + result.missedFrames, copiedEnd) - 1);
    }
    ASFW::Protocols::Audio::AMDTP::PacketSlotSnapshot audible{};
    if (timeline.SnapshotSlotForAudioFrameNear(lastAudible, reachable, audible)) {
        result.lastAudiblePacket = audible.packetIndex;
    } else {
        // No packet owns it yet: one bus cycle per rate / 8000 frames.
        constexpr uint64_t kCyclesPerSecond = 8000;
        const uint64_t cycles = rate != 0 ? ((end - fadeStart) * kCyclesPerSecond + rate - 1) / rate : 0;
        result.lastAudiblePacket = std::max(result.lastAudiblePacket, reachable + cycles);
    }
    return result;
}

} // namespace ASFW::Audio::DriverKit
