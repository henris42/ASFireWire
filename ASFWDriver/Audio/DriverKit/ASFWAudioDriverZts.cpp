//
// ASFWAudioDriverZts.cpp
// ASFWDriver
//
// Zero-timestamp publication: the one call into UpdateCurrentZeroTimestamp
// (PublishSharedZeroTimestampToHAL), the RX anchor action, the M-Audio TX
// clock observation, and the StartIO clock-domain / timeline-epoch choice.
// documentation/HARDWARE_TIMELINE_OWNERSHIP.md. The TX producer lives in
// ASFWAudioDriverTxProducer.cpp.
//

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <array>
#include <span>

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"
#include "../Wire/IEC61883/Syt.hpp"
#include "../Families/BeBoB/MAudio/MAudioClockSourcePolicy.hpp"

#include <DriverKit/DriverKit.h>

namespace ASFW::Audio::DriverKit {

ASFW::Audio::Runtime::ZtsMirrorPublishResult PublishSharedZeroTimestampToHAL(
    ASFWAudioDriver_IVars& ivars,
    const char* reason,
    bool logSuccess) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    auto* audioDevice = ivars.audioDevice.get();
    if (!control || !audioDevice) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::NotReady;
    }

    const uint64_t lastGeneration =
        ivars.runtime.lastHalZeroTimestampGeneration.load(
            std::memory_order_acquire);
    ASFW::Audio::Runtime::HostClockAnchorSample anchor{};
    if (!control->hostClockAnchor.TryReadLatest(
            lastGeneration, anchor)) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::
            NoNewGeneration;
    }
    // An anchor projected in an epoch that has since ended (StartIO, loss)
    // describes a mapping the timeline no longer holds; never hand it to the
    // HAL. Untagged anchors (no epoch) predate the timeline and pass.
    const uint64_t liveEpoch = control->hardwareTimeline.Epoch();
    if (anchor.timelineEpoch != 0 && anchor.timelineEpoch != liveEpoch) {
        ivars.runtime.lastHalZeroTimestampGeneration.store(
            anchor.generation, std::memory_order_release);
        ASFW_LOG_RL(DirectAudio, "zts/stale-epoch", 1000, Warning,
                    "[Zts] stale anchor refused epoch=%llu live=%llu frame=%llu",
                    anchor.timelineEpoch, liveEpoch, anchor.sampleFrame);
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::StaleEpoch;
    }

    const uint64_t previousHost =
        ivars.runtime.lastHalZeroTimestampHostTicks.load(std::memory_order_relaxed);
    const uint64_t previousFrame =
        ivars.runtime.lastHalZeroTimestampSampleFrame.load(std::memory_order_relaxed);
    const bool firstPublication = previousHost == 0;
    audioDevice->UpdateCurrentZeroTimestamp(
        anchor.sampleFrame, anchor.hostTicks);
    ivars.runtime.lastHalZeroTimestampPublishTicks.store(mach_absolute_time(),
                                                         std::memory_order_relaxed);
    // How far this zero timestamp lands from the previous one projected at the
    // nominal rate: CoreAudio's clock for the device takes every step. Word
    // clock against the host clock moves it a few microseconds per period; a
    // step of a packet's worth or more is the timestamps', not the clock's.
    const uint32_t rate = ivars.runtime.directAudioGraph.sampleRateHz;
    if (!firstPublication && rate != 0 && anchor.sampleFrame > previousFrame) {
        const int64_t expectedNanos = static_cast<int64_t>(
            (anchor.sampleFrame - previousFrame) * 1'000'000'000ULL / rate);
        const int64_t actualNanos =
            static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(anchor.hostTicks)) -
            static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(previousHost));
        const int64_t jump = actualNanos - expectedNanos;
        auto& jumpMin = ivars.runtime.ztsJumpMinNanos;
        auto& jumpMax = ivars.runtime.ztsJumpMaxNanos;
        if (jump < jumpMin.load(std::memory_order_relaxed)) jumpMin.store(jump, std::memory_order_relaxed);
        if (jump > jumpMax.load(std::memory_order_relaxed)) jumpMax.store(jump, std::memory_order_relaxed);
        constexpr int64_t kZtsJumpWarnNanos = 500'000;
        if (jump > kZtsJumpWarnNanos || jump < -kZtsJumpWarnNanos) {
            ASFW_LOG_RL(DirectAudio, "zts/jump", 5000, Warning,
                        "[Zts] unit 0x%016llx: zero timestamp for frame %llu lands %lld us from the "
                        "previous one (frame %llu) at the nominal rate",
                        ivars.device.guid, anchor.sampleFrame, static_cast<long long>(jump / 1000),
                        previousFrame);
        }
    }
    ivars.runtime.lastHalZeroTimestampSampleFrame.store(
        anchor.sampleFrame, std::memory_order_relaxed);
    ivars.runtime.lastHalZeroTimestampHostTicks.store(
        anchor.hostTicks, std::memory_order_relaxed);
    ivars.runtime.lastHalZeroTimestampGeneration.store(
        anchor.generation, std::memory_order_release);
    control->hostClockAnchor.mirrorPublications.fetch_add(
        1, std::memory_order_relaxed);
    control->counters.CountRxAdkZtsPublished();

    if (logSuccess) {
        ASFW_LOG(
            DirectAudio,
            "ADK ZTS publish reason=%{public}s generation=%llu sample=%llu host=%llu adkPeriod=%u",
            reason ? reason : "unknown",
            anchor.generation,
            anchor.sampleFrame,
            anchor.hostTicks,
            audioDevice->GetZeroTimestampPeriod());
    }

    if (firstPublication) {
        ASFW_LOG_DEBUG(
            DirectAudio,
            "Core audio hardware ZTS ready guid=0x%016llx sampleFrame=%llu hostTicks=%llu",
            ivars.device.guid,
            anchor.sampleFrame,
            anchor.hostTicks);
    }
    return ASFW::Audio::Runtime::ZtsMirrorPublishResult::Published;
}

// The M-Audio Transmit clock: TX completion stamps become observations on the
// device's timeline, and its boundaries reach the HAL through the anchor
// mailbox. Runs on the TX preparation queue (TxPreparationReady).
void ObserveMAudioTxClock(ASFWAudioDriver_IVars& ivars,
                          const uint64_t transportGeneration) noexcept {
    auto* queue = ivars.runtime.txSlotProvider.queueControl;
    auto* control = ivars.runtime.directAudioGraph.control;
    if (!queue || !control || !ivars.runtime.mAudioInternalTxActive.load(
                                  std::memory_order_acquire) ||
        transportGeneration == 0) {
        return;
    }

    const uint64_t stampCount =
        queue->completionStampCount.load(std::memory_order_acquire);
    if (stampCount == 0) {
        ivars.runtime.txCompletionStampCursor = 0;
        return;
    }

    constexpr uint32_t kStampCapacity =
        ASFW::Isoch::kIsochTxCompletionStampSlots;
    const auto drain = ASFW::Audio::Runtime::PlanTxCompletionStampDrain(
        ivars.runtime.txCompletionStampCursor, stampCount, kStampCapacity);
    if (drain.queueRestarted) {
        ivars.runtime.mAudioTxCorrelationUnwrap = {};
        ivars.runtime.txCompletionStampCursor = stampCount;
        return;
    }
    if (drain.missed != 0) {
        const uint64_t failures =
            ivars.runtime.mAudioTxClockConversionFailures.fetch_add(
                drain.missed, std::memory_order_relaxed) + drain.missed;
        if ((failures & (failures - 1)) == 0) {
            ASFW_LOG_ERROR(
                DirectAudio,
                "[MAudioTxClock] completion stamps missed=%llu total=%llu",
                drain.missed, failures);
        }
    }

    ASFW::Isoch::IsochTxClockPairSample pair{};
    if (!queue->clockPair.TryRead(pair) || pair.hostTimeMid == 0) {
        ivars.runtime.txCompletionStampCursor = stampCount;
        const uint64_t wakes =
            ivars.runtime.mAudioTxClockNoDataWakes.fetch_add(
                1, std::memory_order_relaxed) + 1;
        if ((wakes & (wakes - 1)) == 0) {
            ASFW_LOG_ERROR(DirectAudio,
                           "[MAudioTxClock] no correlation pair wakes=%llu",
                           wakes);
        }
        return;
    }

    std::array<ASFW::Audio::Families::BeBoB::MAudio::
                   TxDataClockObservation,
               ASFW::Isoch::kIsochTxCompletionStampSlots>
        dataPackets{};
    size_t dataPacketCount = 0;
    uint64_t newestCompletionBusTicks = 0;
    uint64_t correlationBusTicks = 0;
    for (uint64_t stampIndex = drain.first;
         stampIndex < drain.last;
         ++stampIndex) {
        uint64_t packetIndex = 0;
        uint32_t completionCycleTimer = 0;
        if (!queue->ReadCompletionStamp(stampIndex, packetIndex,
                                        completionCycleTimer)) {
            continue;
        }
        uint64_t completionTicks = 0;
        uint64_t correlationTicks = 0;
        if (!ASFW::Audio::Shared::ExpandCompletionAgainstCorrelation(
                ivars.runtime.mAudioTxCorrelationUnwrap,
                completionCycleTimer, pair.cycleTimer32,
                completionTicks, correlationTicks)) {
            const uint64_t failures =
                ivars.runtime.mAudioTxClockConversionFailures.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            if ((failures & (failures - 1)) == 0) {
                ASFW_LOG_ERROR(
                    DirectAudio,
                    "[MAudioTxClock] stamp conversion failed total=%llu stamp=%llu cycle=0x%08x correlation=0x%08x",
                    failures, stampIndex, completionCycleTimer,
                    pair.cycleTimer32);
            }
            continue;
        }
        newestCompletionBusTicks = completionTicks;
        correlationBusTicks = correlationTicks;

        const auto* slot = ivars.runtime.txStreamEngine.Timeline().SlotByIndex(packetIndex);
        if (!slot || !slot->isData || slot->framesInPacket == 0 ||
            dataPacketCount == dataPackets.size()) {
            continue;
        }
        dataPackets[dataPacketCount++] = {
            .completionBusTicks = completionTicks,
            .correlationBusTicks = correlationTicks,
            .sampleFrame = slot->firstAudioFrame,
            .frameCount = slot->framesInPacket,
            .sytOffsetTicks =
                ASFW::Audio::Families::BeBoB::MAudio::
                    SytOffsetTicksForPacketIndex(packetIndex),
        };
    }
    ivars.runtime.txCompletionStampCursor = stampCount;
    if (newestCompletionBusTicks == 0 || correlationBusTicks == 0) {
        const uint64_t wakes =
            ivars.runtime.mAudioTxClockNoDataWakes.fetch_add(
                1, std::memory_order_relaxed) + 1;
        if ((wakes & (wakes - 1)) == 0) {
            ASFW_LOG_ERROR(
                DirectAudio,
                "[MAudioTxClock] no usable completion stamps wakes=%llu count=%llu",
                wakes, stampCount);
        }
        return;
    }

    const auto boundary = ivars.runtime.mAudioTxClockBridge.ObserveWake(
        transportGeneration, pair.cycleTimer32, correlationBusTicks,
        pair.hostTimeMid, newestCompletionBusTicks,
        std::span<const ASFW::Audio::Families::BeBoB::MAudio::
                      TxDataClockObservation>(dataPackets.data(),
                                              dataPacketCount));
    if (!boundary.ready) {
        return;
    }

    if (boundary.boundary.hostTicks == 0) {
        return;
    }
    // Same path as the RX clock: the timeline's boundary goes into the anchor
    // mailbox, and PublishSharedZeroTimestampToHAL is the one place that calls
    // UpdateCurrentZeroTimestamp (documentation/HARDWARE_TIMELINE_OWNERSHIP.md).
    const auto published = control->PublishHostClockAnchor(
        boundary.boundary.sampleFrame, boundary.boundary.hostTicks,
        boundary.boundary.hostNanosPerSampleQ8, boundary.boundary.epoch);
    if (!published.accepted) {
        return;
    }
    if (PublishSharedZeroTimestampToHAL(ivars, "maudio-tx", false) ==
        ASFW::Audio::Runtime::ZtsMirrorPublishResult::Published) {
        control->counters.CountZtsPublished();
    }
}

bool SelectTxClockDomain(ASFWAudioDriver_IVars& ivars,
                         const ASFW::Isoch::Audio::IAudioStreamProfile& profile) noexcept {
    ivars.runtime.mAudioInternalTxActive =
        profile.TransmitClockSource() == ASFW::Isoch::Audio::TxClockSource::kInternalCadence;
    ivars.runtime.rxReplayAfterBootstrap.store(
        profile.TransmitClockSource() ==
            ASFW::Isoch::Audio::TxClockSource::kRxReplayAfterBootstrap,
        std::memory_order_release);
    const bool bootstrapProfile = ivars.runtime.rxReplayAfterBootstrap.load(
        std::memory_order_acquire);
    // The start's timeline epoch. A Transmit clock (M-Audio internal cadence)
    // begins its own when the TX clock bridge arms; every other device takes
    // its clock from RX. A rate outside the HAL ladder gets no epoch, and RX
    // anchors stay untagged as before.
    if (!ivars.runtime.mAudioInternalTxActive) {
        if (auto* control = ivars.runtime.directAudioGraph.control) {
            const uint32_t rateHz = static_cast<uint32_t>(ivars.device.currentSampleRate);
            const uint64_t epoch = control->hardwareTimeline.BeginEpoch(
                ASFW::Audio::Runtime::HardwareTimelineSource::Receive,
                ASFW::Audio::Runtime::HardwareTimelineDiscontinuity::StartIO, rateHz, 0);
            if (epoch == 0) {
                ASFW_LOG_ERROR(DirectAudio, "[StopTrace] stage=start-epoch rate=%u action=refuse-busy-or-invalid-timeline", rateHz);
                return false;
            }
            ASFW_LOG_DEBUG(DirectAudio, "[Zts] epoch=%llu source=receive reason=start-io rate=%u",
                           epoch, rateHz);
        }
    }
    ivars.runtime.mAudioTxClockProfile.store(
        ivars.runtime.mAudioInternalTxActive.load(
            std::memory_order_acquire),
        std::memory_order_release);
    return !(bootstrapProfile &&
             static_cast<uint32_t>(ivars.device.currentSampleRate) != 48000U) &&
           !(ivars.runtime.mAudioInternalTxActive &&
             (!ivars.runtime.mAudioInternalTxTiming.Arm() ||
              static_cast<uint32_t>(ivars.device.currentSampleRate) != 48000U));
}

} // namespace ASFW::Audio::DriverKit

void IMPL(ASFWAudioDriver, ZtsAnchorReady)
{
    (void)action;
    (void)generation;
    if (!ivars || !ivars->audioDevice) {
        return;
    }

    // M-Audio's clock is qualified from host TX completion stamps. An RX
    // anchor must not race that source or release StartIO with the wrong clock.
    if (!ASFW::Audio::Families::BeBoB::MAudio::ShouldMirrorRxClockAnchor(
            ivars->runtime.mAudioTxClockProfile.load(
                std::memory_order_acquire))) {
        return;
    }

    (void)ASFW::Audio::DriverKit::PublishSharedZeroTimestampToHAL(
        *ivars, "rx-action", false);
}
