//
// ASFWAudioDriverTxProducer.cpp
// ASFWDriver
//
// The TX producer: packet preparation (PrepareTransmitSlots), the start
// prefill, arming the primary producer, and the TX preparation wake. Split
// from ASFWAudioDriverZts.cpp (Epic 4, T6); convergence milestone 6 (FW-209)
// redesigns this side.
//

#include <algorithm>
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

// A producer stall up to the preparation slack is survivable by the ring; the
// RX replay history must outlast it too, or the reader re-anchors and the lead
// cannot recover (TX_OWNERSHIP.md, T5b).
static_assert(ASFW::Audio::Runtime::RxSequenceReplayState::kCapacity -
                      ASFW::Audio::Runtime::RxSequenceReplayState::kReadDelay >=
                  ASFW::IsochTransport::AudioTimingGeometry::kTxPreparationSlackPackets,
              "RX replay history must outlast a producer stall the IT ring survives");

namespace ASFW::Audio::DriverKit {

// The TX frame cursor's start: the RX packet's first frame plus the
// presentation distance to the TX packet, in frames at the live rate. 44.1k
// has no integer ticks per sample, so the tick*rate product is divided.
// It is NOT rounded to a packet boundary: rounding down made every output
// frame play 0-7 frames late, chosen by each start's phase (measured on the
// Pro 24 DSP: RTL_ts = 105.03 + the rounding, TX_OWNERSHIP.md §1g). Nothing
// downstream needs packet-aligned frames.
uint64_t ProjectTxFrameCursor(uint64_t rxFirstFrame,
                              uint64_t presentationDeltaTicks,
                              uint32_t sampleRate) noexcept {
    return rxFirstFrame +
           (presentationDeltaTicks * sampleRate) / ASFW::Timing::kTicksPerSecond;
}

namespace {

// MOTU SPH regulator telemetry. Every line reaches the unified log, so a
// Release build can be read with:
//
//   /usr/bin/log show --last 10m --info --style compact \
//       --predicate 'eventMessage CONTAINS "[MotuSph"'
//
//   [MotuSphServo]  summary, with the [TxPrep] heartbeat (every 5 s)
//   [MotuSphState]  the counters behind it, same cadence
//   [MotuSphLock]   the first lock of a start or of a new stream generation,
//                   and how long the output was muted for it
//   [MotuSphSeed]   the first absolute phase `rel` of a generation reached the
//                   loop, which seeded its error against the model's setpoint
//   [MotuSphHard]   a hard resync: mute, then a one-shot phase repair
//   [MotuSphMute]   any other change of the regulator's output mute
//   [MotuSphStall]  the RX SPH meter fell behind its bridge (logged by
//                   MotuRxTimingObserver)
//
// The edge lines come from the servo hook below, once per event; nothing is
// logged per packet. The receive side of the absolute phase logs its own
// lines from MotuRxTimingObserver's drain ([RxSphRate], [RxPhaseAbs],
// [RxPhaseRel], ...); all of them together:
//
//   /usr/bin/log show --last 10m --info --style compact --predicate \
//       'eventMessage CONTAINS "[MotuSph" OR eventMessage CONTAINS "[RxPhase" OR
//        eventMessage CONTAINS "[RxSphRate]"'

using ASFW::Audio::Wire::MotuSphRatePpb;

void LogMotuSphEdges(ASFWAudioDriver_IVars& ivars,
                     const ASFW::Audio::Wire::MotuSphServoApplyResult& result,
                     const ASFW::Audio::Runtime::MotuRxSphClockSample& rxClock) noexcept {
    auto& runtime = ivars.runtime;
    if (!result.hardResyncStarted && !result.acquisitionRestarted && !result.unmuteActivated) {
        return;
    }
    const auto servo = runtime.motuTxTimingStamper.ServoTelemetrySnapshot();
    if (result.acquisitionRestarted) {
        runtime.motuSphAcquireHostTicks = mach_absolute_time();
        runtime.motuSphLockPending = true;
        ASFW_LOG(DirectAudio, "[MotuSphMute] gen=%llu muted=1 cause=new-generation",
                 servo.streamGeneration);
    }
    if (result.hardResyncStarted) {
        ASFW_LOG(DirectAudio,
                 "[MotuSphHard] gen=%llu err=%lld repair=%u muted=%u requests=%llu "
                 "repairs=%llu rxFrames=%llu",
                 servo.streamGeneration, static_cast<long long>(servo.phaseErrorTicks),
                 result.phaseRepairApplied ? 1u : 0u, result.outputMuted ? 1u : 0u,
                 servo.hardResyncRequests, servo.phaseRepairs, servo.rxFrames);
    }
    if (result.unmuteActivated) {
        if (runtime.motuSphLockPending) {
            runtime.motuSphLockPending = false;
            const uint64_t now = mach_absolute_time();
            const uint64_t elapsed = now >= runtime.motuSphAcquireHostTicks
                                         ? now - runtime.motuSphAcquireHostTicks
                                         : 0;
            // The servo's inputs too: a rate estimate far from nominal (hardware,
            // 2026-10-18: -1 150 000 ppb on every lock) must be traceable to
            // the step the device's SPH gave against the nominal, and to the
            // bridge sample (device frames vs cycle-timer ticks) it came from.
            ASFW_LOG_DEBUG(DirectAudio,
                           "[MotuSphLock] gen=%llu afterUs=%llu decisions=%llu err=%lld "
                           "ratePpb=%lld lead=%u nomStep=%lld step=%lld meas=%lld rxFr=%llu rxTk=%llu",
                           servo.streamGeneration, ASFW::Timing::hostTicksToNanos(elapsed) / 1000,
                           servo.decisions, static_cast<long long>(servo.phaseErrorTicks),
                           static_cast<long long>(MotuSphRatePpb(servo.stepQ32, servo.nominalStepQ32)),
                           runtime.motuTxTimingStamper.PresentationLeadTicks(),
                           static_cast<long long>(servo.nominalStepQ32),
                           static_cast<long long>(servo.stepQ32),
                           static_cast<long long>(servo.measuredStepQ32),
                           static_cast<unsigned long long>(rxClock.rxFrames),
                           static_cast<unsigned long long>(rxClock.rxTicks));
        } else {
            ASFW_LOG(DirectAudio, "[MotuSphMute] gen=%llu muted=0 cause=relocked err=%lld",
                     servo.streamGeneration, static_cast<long long>(servo.phaseErrorTicks));
        }
    }
}

// The CIP header's SID is our node id, which the session publishes in the
// control block when it prepares the transmit context (RestartRoutine) and
// which a bus reset can change. Before every packet, so the prefill and the
// first live packets carry it too, and a restart on a new node id as well.
void ApplyCipSourceId(ASFWAudioDriver_IVars& ivars,
                      const ASFW::Audio::Runtime::AudioTransportControlBlock& control) noexcept {
    const uint32_t sid = control.cipSourceId.load(std::memory_order_acquire);
    if (sid == ASFW::Audio::Runtime::AudioTransportControlBlock::kNoSourceId) {
        return;
    }
    ivars.runtime.txStreamEngine.SetSourceId(static_cast<uint8_t>(sid));
    if (ivars.runtime.txSecondaryActive) {
        ivars.runtime.txStreamEngineSecondary.SetSourceId(static_cast<uint8_t>(sid));
    }
}

// The servo input hook, run before EVERY packet the producer prepares, NO-DATA
// included: it hands the stamper the latest generation-checked RX clock
// sample and keeps the payload writer on the regulator's mute.
void ApplyMotuSphServoInput(ASFWAudioDriver_IVars& ivars,
                            ASFW::Audio::Runtime::AudioTransportControlBlock& control) noexcept {
    auto& stamper = ivars.runtime.motuTxTimingStamper;

    ASFW::Audio::Runtime::MotuRxSphClockSample rxClock{};
    uint64_t rxClockUpdates = 0;
    const bool haveRxClock = control.motuRxSphClock.ReadLatest(rxClock, rxClockUpdates);
    const uint64_t controlGeneration = control.generation.load(std::memory_order_acquire);

    // The conditioned absolute phase, from its own bridge, where the model has
    // a setpoint at this rate (48 kHz); elsewhere the loop stays relative.
    // Generation-checked like the clock sample: a `rel` measured in another
    // stream describes a phase this one never had.
    ASFW::Audio::Runtime::MotuRelPhaseSampleRecord relPhase{};
    uint64_t relPhaseUpdates = 0;
    const bool haveRelPhase = stamper.HoldsAbsolutePhase() &&
                              control.motuRelPhase.ReadLatest(relPhase, relPhaseUpdates) &&
                              relPhase.streamGeneration == controlGeneration;

    const auto result = stamper.ApplyServoInput({
        .valid = haveRxClock,
        .streamGeneration = rxClock.streamGeneration,
        .expectedStreamGeneration = controlGeneration,
        .bridgeUpdates = rxClockUpdates,
        .rxFrames = rxClock.rxFrames,
        .rxTicks = rxClock.rxTicks,
        .haveRelPhase = haveRelPhase,
        .relPhaseTicks = relPhase.ticks,
        .relPhaseCenterTicks = relPhase.centerTicks,
    });
    // The gate's state, not the result's: a dropped or early observation
    // leaves the result without a decision while the gate may still be closed.
    ivars.runtime.motuPayloadWriter.SetPcmMuted(stamper.IsOutputMuted());
    LogMotuSphEdges(ivars, result, rxClock);
    if (result.absolutePhaseAcquired) {
        ASFW_LOG(DirectAudio, "[MotuSphSeed] gen=%llu rel=%lld center=%lld setpoint=%d err=%lld",
                 controlGeneration, static_cast<long long>(relPhase.ticks),
                 static_cast<long long>(relPhase.centerTicks),
                 stamper.AbsolutePhaseSetpointTicks(),
                 static_cast<long long>(result.decision.phaseErrorTicks));
    }
}

// The summary, on the [TxPrep] heartbeat. Two short lines: each must fit the
// driver log ring's 232-byte record together with its category prefix.
// ratePpb/rxPpb: the transmit step and the device's measured period against
// nominal (positive = faster). same: observations dropped as repeats (benign);
// back: bridge/frame/tick counters seen going backwards (never expected).
// trace: cadence periods the phase-trace bridge took / refused this start
// (a refusal means another stream generation, never expected).
// [Glitch]: lost audio at the default (warn) level. Output frames that
// reached their packet too late (missedFinality) and capture reads that came
// up short (starvations after start-up: the first complete read and 2 s more,
// ASFWAudioDriverIO.cpp) are what the user hears; the line
// adds what explains them: the longest fill and the longest producer wake of
// the current heartbeat interval, and the BeginRead pulls since the last
// report (RxPullHook.hpp). At most one line per kGlitchReportIntervalNanos per
// unit; what happens in between goes into the next line.
namespace {

const char* PullOutcomeName(ASFW::Audio::Runtime::RxPullOutcome outcome) noexcept {
    using ASFW::Audio::Runtime::RxPullOutcome;
    switch (outcome) {
    case RxPullOutcome::NoPull: return "no-pull";
    case RxPullOutcome::Covered: return "covered";
    case RxPullOutcome::NotArrived: return "not-arrived";
    case RxPullOutcome::Cut: return "cut";
    case RxPullOutcome::Busy: return "busy";
    }
    return "?";
}

// The events behind a [Glitch] report (GlitchEventLog.hpp), oldest first, at
// most kGlitchEventLines per report: each at its frame in the stream (@,
// CoreAudio's sample time, the frames since the stream started that a
// client's IOProc sees) and its age (to line up with coreaudiod's overload
// reports). A short read says what the pull found and where the read sat on
// our zero timestamps ("lead": negative = asked for frames not yet arrived).
void LogGlitchEvents(ASFWAudioDriver_IVars& ivars, uint64_t now) noexcept {
    using ASFW::Audio::Runtime::GlitchEvent;
    using ASFW::Audio::Runtime::GlitchEventKind;
    constexpr uint32_t kGlitchEventLines = 16;
    std::array<GlitchEvent, kGlitchEventLines> events{};
    uint64_t lost = 0;
    auto& log = ivars.runtime.glitchEvents;
    const uint32_t count = log.Take(events.data(), kGlitchEventLines, lost);
    const uint32_t rate = ivars.runtime.directAudioGraph.sampleRateHz;
    for (uint32_t index = 0; index < count; ++index) {
        const GlitchEvent& event = events[index];
        const uint64_t positionMs = rate != 0 ? event.sampleFrame * 1000 / rate : 0;
        const uint64_t agoMs =
            now > event.hostTicks ? ASFW::Timing::hostTicksToNanos(now - event.hostTicks) / 1'000'000 : 0;
        if (event.kind == GlitchEventKind::MissedOutputFrames) {
            // Where the write stood (GlitchEvent, output meanings): its first
            // frame's packet behind the finality frontier; the HAL's margin at
            // WriteEnd entry (its presentation time for that frame minus the
            // entry; the HAL finishes its write at least 500 us before, b190);
            // the fill's time; the clock pair's age (a late refill) and the
            // packets sent that no refill had harvested.
            ASFW_LOG_WARNING(DirectAudio,
                             "[GlitchEvent] 0x%016llx @%llu (%llu.%03llu s, %llu ms ago): %u output frames "
                             "missed their packet: %u-frame write %d pk behind the frontier, HAL margin "
                             "%d us at entry, fill %d us, refill %d us ago, %u pk unharvested, thr %04x",
                             ivars.device.guid, event.sampleFrame, positionMs / 1000, positionMs % 1000,
                             agoMs, event.frames, event.missing, event.gapBefore, event.leadUs,
                             event.skewUs, event.prevUs, event.drained, event.thread);
            continue;
        }
        if (event.kind == GlitchEventKind::ReadAhead) {
            // Debug: b193 showed these reads are of the output region (two
            // clients on a unit; the recording is clean at their positions).
            ASFW_LOG_DEBUG(DirectAudio,
                             "[GlitchEvent] 0x%016llx @%llu (%llu.%03llu s, %llu ms ago): read ahead of the "
                             "arrivals: %u frames, the first %d past the write position; lead %d us, skew "
                             "%d us, prev %d fr/%d us, zts %u ms, thr %04x",
                             ivars.device.guid, event.sampleFrame, positionMs / 1000, positionMs % 1000,
                             agoMs, event.frames, event.gapBefore - static_cast<int32_t>(event.frames),
                             event.leadUs, event.skewUs, event.prevFrames, event.prevUs,
                             event.ztsAgeUs / 1000, event.thread);
            continue;
        }
        ASFW_LOG_WARNING(DirectAudio,
                         "[GlitchEvent] 0x%016llx @%llu (%llu.%03llu s, %llu ms ago): short read %u, %u "
                         "missing (%d before pull, %{public}s, %u pk); lead %d us, skew %d us, prev %d "
                         "fr/%d us, zts %u ms, thr %04x",
                         ivars.device.guid, event.sampleFrame, positionMs / 1000, positionMs % 1000, agoMs,
                         event.frames, event.missing, event.gapBefore, PullOutcomeName(event.pull),
                         event.drained, event.leadUs, event.skewUs, event.prevFrames, event.prevUs,
                         event.ztsAgeUs / 1000, event.thread);
    }
    if (const uint64_t more = log.Pending(); more != 0 || lost != 0) {
        ASFW_LOG_WARNING(DirectAudio,
                         "[GlitchEvent] 0x%016llx: %llu more wait for the next report, %llu overwritten",
                         ivars.device.guid, more, lost);
    }
    // Debug: the IO operations up to the first read ahead since the last
    // report, oldest first, each against that read's entry time; "hal" is the
    // HAL's host time for the operation's first frame against the entry
    // (negative: already presented, an input read; positive: still to come).
    if (ASFW_LOG_ENABLED(DirectAudio, Debug)) {
        using ASFW::Audio::Runtime::IoOp;
        using ASFW::Audio::Runtime::IoOpTraceCapture;
        std::array<IoOp, IoOpTraceCapture::kMaxOps> ops{};
        const uint32_t opCount = ivars.runtime.ioOpTraceCapture.Take(ops.data(), ops.size());
        const auto microsFrom = [](uint64_t later, uint64_t earlier) noexcept {
            const uint64_t nanos = ASFW::Timing::hostTicksToNanos(later >= earlier ? later - earlier
                                                                                  : earlier - later);
            const auto micros = static_cast<long long>(nanos / 1000);
            return later >= earlier ? micros : -micros;
        };
        if (opCount != 0) {
            const uint64_t at = ops[opCount - 1].entryTicks;
            for (uint32_t index = 0; index < opCount; ++index) {
                const IoOp& op = ops[index];
                ASFW_LOG_DEBUG(DirectAudio,
                               "[IoOps] 0x%016llx %+lld us: %{public}s @%llu %u fr, hal %+lld us, took %u us, "
                               "thr %04x",
                               ivars.device.guid, microsFrom(op.entryTicks, at),
                               op.operation == IOUserAudioIOOperationBeginRead ? "BeginRead"
                               : op.operation == IOUserAudioIOOperationWriteEnd ? "WriteEnd"
                                                                                : "op?",
                               op.sampleTime, op.frames,
                               op.hostTime != 0 ? microsFrom(op.hostTime, op.entryTicks) : 0LL,
                               op.durationNanos / 1000, op.thread);
            }
        }
    }
}

} // namespace

void ReportTransportGlitches(ASFWAudioDriver_IVars& ivars,
                             const ASFW::Audio::Runtime::AudioTransportControlBlock& control,
                             uint64_t now) noexcept {
    constexpr uint64_t kGlitchReportIntervalNanos = 5'000'000'000ULL;
    auto& runtime = ivars.runtime;
    const uint64_t missedTotal =
        runtime.txStreamEngine.PayloadWriterCounters().framesMissedFinality.load(
            std::memory_order_relaxed) +
        runtime.motuPayloadWriter.Counters().framesMissedFinality.load(std::memory_order_relaxed);
    const uint64_t missed = missedTotal >= runtime.txMissedFinalityAtStart
                                ? missedTotal - runtime.txMissedFinalityAtStart
                                : 0;
    const uint64_t starvedAll = control.captureRingStarvations.load(std::memory_order_relaxed);
    const uint64_t starvedStartup =
        control.captureRingStartupStarvations.load(std::memory_order_relaxed);
    const uint64_t starved = starvedAll >= starvedStartup ? starvedAll - starvedStartup : 0;
    const uint64_t ahead = control.captureRingAheadReads.load(std::memory_order_relaxed);
    const bool lost = missed > runtime.glitchReportedMissed || starved > runtime.glitchReportedStarved;
    if (!lost && ahead <= runtime.glitchReportedAhead) {
        return;
    }
    const uint64_t last = runtime.glitchLastReportTicks;
    if (last != 0 && now > last &&
        ASFW::Timing::hostTicksToNanos(now - last) < kGlitchReportIntervalNanos) {
        return;
    }
    if (!lost) {
        // Only reads ahead of the arrivals since the last report: no loss, so
        // no warning. Their events print at debug; otherwise they are dropped
        // here, so they cannot fill the ring (GlitchEventLog) while nothing
        // else is reported.
        runtime.glitchReportedAhead = ahead;
        runtime.glitchLastReportTicks = now;
        if (ASFW_LOG_ENABLED(DirectAudio, Debug)) {
            LogGlitchEvents(ivars, now);
        } else {
            runtime.glitchEvents.Reset();
        }
        return;
    }
    const uint64_t pulls = control.rxPull.pulls.load(std::memory_order_relaxed);
    const uint64_t pullBusy = control.rxPull.busy.load(std::memory_order_relaxed);
    const uint64_t pullCut = control.rxPull.cut.load(std::memory_order_relaxed);
    // fill max: the longest fill since the last report (this report's own
    // window; the [TxPrep] heartbeat keeps its own at debug).
    ASFW_LOG_WARNING(
        DirectAudio,
        "[Glitch] unit 0x%016llx: %llu output frames missed their packet, %llu capture reads "
        "came up short, %llu read ahead of the arrivals; fill max %llu us, producer wake max %llu us, "
        "BeginRead pulls %llu (%llu gave up, %llu cut)",
        ivars.device.guid, missed - std::min(missed, runtime.glitchReportedMissed),
        starved - std::min(starved, runtime.glitchReportedStarved),
        ahead - std::min(ahead, runtime.glitchReportedAhead),
        ASFW::Timing::hostTicksToNanos(
            runtime.glitchFillMaxTicks.exchange(0, std::memory_order_relaxed)) / 1000,
        ASFW::Timing::hostTicksToNanos(
            control.txIntervalPreparationLatencyMaxTicks.load(std::memory_order_relaxed)) / 1000,
        pulls - std::min(pulls, runtime.glitchReportedPulls),
        pullBusy - std::min(pullBusy, runtime.glitchReportedPullBusy),
        pullCut - std::min(pullCut, runtime.glitchReportedPullCut));
    runtime.glitchReportedMissed = missed;
    runtime.glitchReportedStarved = starved;
    runtime.glitchReportedAhead = ahead;
    runtime.glitchReportedPulls = pulls;
    runtime.glitchReportedPullBusy = pullBusy;
    runtime.glitchReportedPullCut = pullCut;
    runtime.glitchLastReportTicks = now;
    LogGlitchEvents(ivars, now);
}

// [IoTime]: the IO handler's own time per operation (IoOpTiming.hpp), which
// CoreAudio waits for inside its IO cycle. Per unit, every
// kIoTimeReportIntervalNanos: a warning when an operation of the interval took
// kSlowIoOpNanos or longer, with the latest of those one by one ([IoSlow]: @
// is CoreAudio's sample time, as in [GlitchEvent]); otherwise at debug. When
// the stream stops, the whole stream at info level (LogIoTimingSummary).
// "waited": pulls that found the interrupt's poll in the receive context and
// spun for it. "after the other unit's": IoOpTiming.hpp, follow.
// When CoreAudio got here and the render between the two operations, with the
// render by its position in 8192 written frames: at info level every interval
// and for the stream.
struct IoTimeFigures {
    ASFW::Audio::Runtime::IoStepSpan beginRead;
    ASFW::Audio::Runtime::IoStepSpan writeEnd;
    ASFW::Audio::Runtime::IoStepSpan pull;
    ASFW::Audio::Runtime::IoStepSpan fill;
    ASFW::Audio::Runtime::IoStepSpan follow;
    uint64_t followMinTicks{UINT64_MAX};  // this stream's
    uint64_t pullWaits{0};
    uint64_t pullBusy{0};
    uint64_t slow{0};
    ASFW::Audio::Runtime::IoOffsetSpan readArrival;
    ASFW::Audio::Runtime::IoOffsetSpan writeMargin;
    ASFW::Audio::Runtime::IoStepSpan render;
    ASFW::Audio::Runtime::IoRenderPhaseSpan renderPhases;
};

// A step for a line: how many, their mean in microseconds with one decimal,
// the longest in microseconds.
struct IoStepLine {
    unsigned long long count;
    unsigned long long avgUs;
    unsigned long long avgTenths;
    unsigned long long maxUs;
};

IoStepLine StepLine(const ASFW::Audio::Runtime::IoStepSpan& span) noexcept {
    const uint64_t avgNanos =
        span.count != 0 ? ASFW::Timing::hostTicksToNanos(span.ticks) / span.count : 0;
    return {span.count, avgNanos / 1000, (avgNanos % 1000) / 100,
            ASFW::Timing::hostTicksToNanos(span.maxTicks) / 1000};
}

// Host ticks, signed, in microseconds.
long long SignedMicros(int64_t ticks) noexcept {
    return ticks >= 0
               ? static_cast<long long>(ASFW::Timing::hostTicksToNanos(static_cast<uint64_t>(ticks)) / 1000)
               : -static_cast<long long>(ASFW::Timing::hostTicksToNanos(static_cast<uint64_t>(-ticks)) / 1000);
}

// When CoreAudio got here, and the client's render: two lines of [IoTime].
void LogIoArrival(const ASFWAudioDriver_IVars& ivars, ::ASFW::Logging::LogLevel level,
                  const char* which, const IoTimeFigures& figures) noexcept {
    using ::ASFW::Logging::LogCategory;
    const auto& read = figures.readArrival;
    const auto& write = figures.writeMargin;
    if (read.count != 0 || write.count != 0) {
        const auto mean = [](const ASFW::Audio::Runtime::IoOffsetSpan& span) noexcept {
            return span.count != 0 ? SignedMicros(span.ticks / static_cast<int64_t>(span.count)) : 0LL;
        };
        ::ASFW::Logging::EmitLine(
            LogCategory::DirectAudio, level,
            "[DirectAudio] [IoTime] 0x%016llx %{public}s: BeginRead began avg %lld us after "
            "CoreAudio's time for its first frame (%lld to %lld); WriteEnd began avg %lld us before "
            "its first frame's presentation (%lld to %lld)",
            ivars.device.guid, which, mean(read), SignedMicros(read.minTicks),
            SignedMicros(read.maxTicks), mean(write), SignedMicros(write.minTicks),
            SignedMicros(write.maxTicks));
    }
    if (figures.render.count == 0) {
        return;
    }
    const IoStepLine render = StepLine(figures.render);
    const auto& phases = figures.renderPhases;
    const auto meanNanos = [](uint64_t ticks, uint64_t count) noexcept {
        return count != 0 ? ASFW::Timing::hostTicksToNanos(ticks) / count : 0ULL;
    };
    const unsigned long long high = meanNanos(phases.highTicks, phases.highCount);
    const unsigned long long low = meanNanos(phases.lowTicks, phases.lowCount);
    ::ASFW::Logging::EmitLine(
        LogCategory::DirectAudio, level,
        "[DirectAudio] [IoTime] 0x%016llx %{public}s: render (BeginRead's return to WriteEnd) %llu, "
        "avg %llu.%llu us, max %llu us; by position in %u written frames, %u positions: highest "
        "mean %llu.%llu us at frame %u (%llu renders), lowest %llu.%llu us at frame %u",
        ivars.device.guid, which, render.count, render.avgUs, render.avgTenths, render.maxUs,
        ASFW::Audio::Runtime::kRenderPhaseFrames, phases.positions, high / 1000, (high % 1000) / 100,
        phases.highFrame, static_cast<unsigned long long>(phases.highCount), low / 1000,
        (low % 1000) / 100, phases.lowFrame);
}

void LogIoTime(const ASFWAudioDriver_IVars& ivars, ::ASFW::Logging::LogLevel level,
               const char* which, uint64_t spanTicks, const IoTimeFigures& figures) noexcept {
    using ::ASFW::Logging::LogCategory;
    using ::ASFW::Logging::LogLevel;
    // The arrival and render lines go out at info when the rest is at debug.
    if (const LogLevel arrivalLevel = level == LogLevel::Debug ? LogLevel::Info : level;
        ::ASFW::Logging::Allows(LogCategory::DirectAudio, arrivalLevel)) {
        LogIoArrival(ivars, arrivalLevel, which, figures);
    }
    if (!::ASFW::Logging::Allows(LogCategory::DirectAudio, level)) {
        return;
    }
    const unsigned long long spanMs = ASFW::Timing::hostTicksToNanos(spanTicks) / 1'000'000;
    const IoStepLine beginRead = StepLine(figures.beginRead);
    const IoStepLine pull = StepLine(figures.pull);
    const IoStepLine writeEnd = StepLine(figures.writeEnd);
    const IoStepLine fill = StepLine(figures.fill);
    ::ASFW::Logging::EmitLine(
        LogCategory::DirectAudio, level,
        "[DirectAudio] [IoTime] 0x%016llx %{public}s %llu.%llu s: BeginRead %llu, avg %llu.%llu us, "
        "max %llu us; pull %llu, avg %llu.%llu us, max %llu us; %llu waited for the interrupt's "
        "poll, %llu gave up",
        ivars.device.guid, which, spanMs / 1000, (spanMs % 1000) / 100, beginRead.count,
        beginRead.avgUs, beginRead.avgTenths, beginRead.maxUs, pull.count, pull.avgUs,
        pull.avgTenths, pull.maxUs, static_cast<unsigned long long>(figures.pullWaits),
        static_cast<unsigned long long>(figures.pullBusy));
    ::ASFW::Logging::EmitLine(
        LogCategory::DirectAudio, level,
        "[DirectAudio] [IoTime] 0x%016llx %{public}s: WriteEnd %llu, avg %llu.%llu us, max %llu us; "
        "fill %llu, avg %llu.%llu us, max %llu us; %llu operations took %llu us or more",
        ivars.device.guid, which, writeEnd.count, writeEnd.avgUs, writeEnd.avgTenths,
        writeEnd.maxUs, fill.count, fill.avgUs, fill.avgTenths, fill.maxUs,
        static_cast<unsigned long long>(figures.slow),
        static_cast<unsigned long long>(ASFW::Audio::Runtime::kSlowIoOpNanos / 1000));
    if (figures.follow.count == 0) {
        return;
    }
    const IoStepLine follow = StepLine(figures.follow);
    const uint64_t leastNanos = figures.followMinTicks != UINT64_MAX
                                    ? ASFW::Timing::hostTicksToNanos(figures.followMinTicks)
                                    : 0;
    ::ASFW::Logging::EmitLine(
        LogCategory::DirectAudio, level,
        "[DirectAudio] [IoTime] 0x%016llx %{public}s: %llu began after the other unit's same "
        "operation returned: avg %llu.%llu us later, at most %llu us; this stream's least %llu.%llu us",
        ivars.device.guid, which, follow.count, follow.avgUs, follow.avgTenths, follow.maxUs,
        static_cast<unsigned long long>(leastNanos / 1000),
        static_cast<unsigned long long>((leastNanos % 1000) / 100));
}

void LogMotuSphSummary(const ASFWAudioDriver_IVars& ivars) noexcept {
    const auto servo = ivars.runtime.motuTxTimingStamper.ServoTelemetrySnapshot();
    ASFW::Audio::Runtime::MotuRxSphClockSample bridge{};
    uint64_t bridgeUpdates = 0;
    const bool haveBridge = ivars.runtime.directAudioGraph.control != nullptr &&
        ivars.runtime.directAudioGraph.control->motuRxSphClock.ReadLatest(bridge, bridgeUpdates);
    ASFW_LOG_DEBUG(DirectAudio,
                   "[MotuSphServo] gen=%llu dec=%llu locked=%u muted=%u seeded=%u err=%lld "
                   "ratePpb=%lld rxPpb=%lld step=%lld corr=%lld bridgeUpd=%llu bridgeGen=%llu bridgeFr=%llu",
                   servo.streamGeneration, servo.decisions, servo.locked ? 1u : 0u,
                   servo.outputMuted ? 1u : 0u, servo.seeded ? 1u : 0u,
                   static_cast<long long>(servo.phaseErrorTicks),
                   static_cast<long long>(MotuSphRatePpb(servo.stepQ32, servo.nominalStepQ32)),
                   static_cast<long long>(MotuSphRatePpb(servo.measuredStepQ32, servo.nominalStepQ32)),
                   static_cast<long long>(servo.stepQ32),
                   static_cast<long long>(servo.txCorrectionQ32),
                   haveBridge ? bridgeUpdates : 0ULL, haveBridge ? bridge.streamGeneration : 0ULL,
                   haveBridge ? bridge.rxFrames : 0ULL);
    ASFW_LOG_DEBUG(DirectAudio,
                   "[MotuSphState] gen=%llu hard=%llu repair=%llu mute=%llu unmute=%llu "
                   "mutedFr=%llu drops=%u same=%u back=%u/%u/%u seed=%u stall=%u trace=%llu/%llu",
                   servo.streamGeneration, servo.hardResyncRequests, servo.phaseRepairs,
                   servo.muteTransitions, servo.unmuteTransitions,
                   ivars.runtime.motuPayloadWriter.Counters().framesMuted.load(
                 std::memory_order_relaxed),
                   servo.observationDrops, servo.dropBridgeNotAdvanced,
                   servo.dropBridgeRegressed, servo.dropRxFramesRegressed,
                   servo.dropRxTicksRegressed, servo.seedReReferences, servo.bridgeStalls,
                   ivars.runtime.motuTxPhaseTrace.Published(),
                   ivars.runtime.motuTxPhaseTrace.Rejected());
}

} // namespace

void ReportIoTiming(ASFWAudioDriver_IVars& ivars,
                    const ASFW::Audio::Runtime::AudioTransportControlBlock& control,
                    uint64_t now) noexcept {
    constexpr uint64_t kIoTimeReportIntervalNanos = 5'000'000'000ULL;
    constexpr uint32_t kIoSlowLines = 8;
    auto& timing = ivars.runtime.ioTiming;
    if (now < timing.nextReportTicks) {
        return;
    }
    const bool first = timing.nextReportTicks == 0;
    timing.nextReportTicks = now + ASFW::Timing::nanosToHostTicks(kIoTimeReportIntervalNanos);
    if (first) {
        return;  // the stream's first interval starts here
    }
    const uint64_t slowTotal = timing.slow.load(std::memory_order_relaxed);
    const uint64_t pullWaits = control.rxPull.waited.load(std::memory_order_relaxed);
    const uint64_t pullBusy = control.rxPull.busy.load(std::memory_order_relaxed);
    const IoTimeFigures figures{
        .beginRead = timing.beginRead.Take(),
        .writeEnd = timing.writeEnd.Take(),
        .pull = timing.pull.Take(),
        .fill = timing.fill.Take(),
        .follow = timing.follow.Take(),
        .followMinTicks = timing.followMinTicks.load(std::memory_order_relaxed),
        .pullWaits = pullWaits - std::min(pullWaits, timing.reportedPullWaits),
        .pullBusy = pullBusy - std::min(pullBusy, timing.reportedPullBusy),
        .slow = slowTotal - std::min(slowTotal, timing.reportedSlow),
        .readArrival = timing.readArrival.Take(),
        .writeMargin = timing.writeMargin.Take(),
        .render = timing.render.Take(),
        // Read only for a line that goes out; a later report covers the rest.
        .renderPhases = ::ASFW::Logging::Allows(::ASFW::Logging::LogCategory::DirectAudio,
                                                ::ASFW::Logging::LogLevel::Info)
                            ? timing.renderPhases.Span(/*take=*/true)
                            : ASFW::Audio::Runtime::IoRenderPhaseSpan{},
    };
    const uint64_t since = timing.lastReportTicks;
    timing.reportedSlow = slowTotal;
    timing.reportedPullWaits = pullWaits;
    timing.reportedPullBusy = pullBusy;
    timing.lastReportTicks = now;
    if (figures.beginRead.count == 0 && figures.writeEnd.count == 0) {
        return;  // no IO in the interval
    }
    LogIoTime(ivars,
              figures.slow != 0 ? ::ASFW::Logging::LogLevel::Warning : ::ASFW::Logging::LogLevel::Debug,
              "last", now - std::min(now, since), figures);

    // The slow operations themselves, the latest kIoSlowLines of the interval.
    using ASFW::Audio::Runtime::IoOp;
    const uint64_t slowOpsTotal = timing.slowOps.Recorded();
    const uint64_t slowOps = slowOpsTotal - std::min(slowOpsTotal, timing.reportedSlowOps);
    timing.reportedSlowOps = slowOpsTotal;
    if (slowOps == 0) {
        return;
    }
    std::array<IoOp, kIoSlowLines> ops{};
    const uint32_t count = timing.slowOps.Snapshot(
        ops.data(), static_cast<uint32_t>(std::min<uint64_t>(slowOps, kIoSlowLines)));
    for (uint32_t index = 0; index < count; ++index) {
        const IoOp& op = ops[index];
        const bool read = op.operation == IOUserAudioIOOperationBeginRead;
        ASFW_LOG_WARNING(DirectAudio,
                         "[IoSlow] 0x%016llx %llu ms ago: %{public}s @%llu, %u frames, took %u us, of it "
                         "%{public}s %u us, thr %04x",
                         ivars.device.guid,
                         now > op.entryTicks
                             ? ASFW::Timing::hostTicksToNanos(now - op.entryTicks) / 1'000'000
                             : 0ULL,
                         read ? "BeginRead" : "WriteEnd", op.sampleTime, op.frames,
                         op.durationNanos / 1000, read ? "the pull" : "the fill", op.partNanos / 1000,
                         op.thread);
    }
    if (slowOps > count) {
        ASFW_LOG_WARNING(DirectAudio, "[IoSlow] 0x%016llx: the latest %u of %llu", ivars.device.guid,
                         count, slowOps);
    }
}

void LogIoTimingSummary(ASFWAudioDriver_IVars& ivars) noexcept {
    auto& timing = ivars.runtime.ioTiming;
    const auto* control = ivars.runtime.directAudioGraph.control;
    const uint64_t pullWaits = control ? control->rxPull.waited.load(std::memory_order_relaxed) : 0;
    const uint64_t pullBusy = control ? control->rxPull.busy.load(std::memory_order_relaxed) : 0;
    const IoTimeFigures figures{
        .beginRead = timing.beginRead.Stream(),
        .writeEnd = timing.writeEnd.Stream(),
        .pull = timing.pull.Stream(),
        .fill = timing.fill.Stream(),
        .follow = timing.follow.Stream(),
        .followMinTicks = timing.followMinTicks.load(std::memory_order_relaxed),
        .pullWaits = pullWaits - std::min(pullWaits, timing.streamStartPullWaits),
        .pullBusy = pullBusy - std::min(pullBusy, timing.streamStartPullBusy),
        .slow = timing.slow.load(std::memory_order_relaxed),
        .readArrival = timing.readArrival.Stream(),
        .writeMargin = timing.writeMargin.Stream(),
        .render = timing.render.Stream(),
        .renderPhases = ::ASFW::Logging::Allows(::ASFW::Logging::LogCategory::DirectAudio,
                                                ::ASFW::Logging::LogLevel::Info)
                            ? timing.renderPhases.Span(/*take=*/false)
                            : ASFW::Audio::Runtime::IoRenderPhaseSpan{},
    };
    if (figures.beginRead.count == 0 && figures.writeEnd.count == 0) {
        return;
    }
    const uint64_t now = mach_absolute_time();
    LogIoTime(ivars, ::ASFW::Logging::LogLevel::Info, "stream",
              now - std::min(now, timing.streamStartTicks), figures);
}

uint32_t PrepareTransmitSlots(ASFWAudioDriver_IVars& ivars,
                             uint64_t startPacketIndex,
                             uint64_t requiredPacketIndex,
                             uint64_t limitPacketIndex,
                             uint32_t maxToPrepare,
                             bool allowRecoveredClock) noexcept {
    const uint32_t numSlots = ivars.runtime.txSlotProvider.numSlots;
    auto* metadataRing = ivars.runtime.txSlotProvider.metadataRing;
    auto* directControl = ivars.runtime.directAudioGraph.control;
    if (directControl == nullptr) {
        ivars.runtime.motuTxTimingStamper.BindReadiness(nullptr);
        return 0;
    }
    // The MOTU stamper reads its RX clock from, and publishes its readiness to,
    // the shared control block -- the seam both services map -- rather than the
    // capture consumer, which IsochDuplexHostTransport owns and destroys on stop
    // while this service may still be preparing packets (the FW-60 cross-service
    // class). Rebind on every pass so the pointer is never older than directControl.
    ivars.runtime.motuTxTimingStamper.BindReadiness(&directControl->transmitTimingReady);
    const bool motuSphActive = ivars.runtime.motuSphActive.load(std::memory_order_relaxed);

    uint64_t nextPacketToPrepare = startPacketIndex;
    uint32_t preparedCount = 0;

    const auto failProducer =
        [&](ASFW::Audio::Runtime::TxProducerFaultStage stage,
            ASFW::Audio::Runtime::TxProducerFaultReason producerReason,
            ASFW::Audio::Runtime::FatalStreamReason runtimeReason,
            uint64_t packetIndex) noexcept {
            auto* txControl =
                ivars.runtime.txSlotProvider.queueControl;
            const uint64_t completionCursor =
                txControl
                    ? txControl->completionCursor.load(
                          std::memory_order_acquire)
                    : 0;
            const uint64_t exposeCursor =
                txControl
                    ? txControl->committedEnd.load(
                          std::memory_order_acquire)
                    : 0;

            ASFW::Audio::Runtime::TxProducerFaultRecord failure{
                .stage = stage,
                .reason = producerReason,
                .packetIndex = packetIndex,
                .rangeStart = startPacketIndex,
                .rangeTarget = limitPacketIndex,
                .preparedCount = preparedCount,
                .completionCursor = completionCursor,
                .committedEnd = exposeCursor,
                .replayProducerCursor =
                    directControl->rxSequenceReplay.ProducerCursor(),
                .replayEpoch =
                    directControl->rxSequenceReplay.Epoch(),
            };
            const uint64_t producerGeneration =
                directControl->txProducerFault.Publish(failure);

            directControl->fatalReason.store(
                runtimeReason, std::memory_order_release);
            const uint64_t runtimeGeneration =
                directControl->fatalGeneration.fetch_add(
                    1, std::memory_order_release) +
                1;
            directControl->counters.txImmediateStops.fetch_add(
                1, std::memory_order_relaxed);

            ASFW_LOG_ERROR(
                DirectAudio,
                "[TxProducerFatal] stage=%{public}s reason=%{public}s "
                "producerGen=%llu runtimeReason=%u runtimeGen=%llu "
                "packet=%llu range=[%llu,%llu) prepared=%u "
                "completion=%llu committedEnd=%llu replayProducer=%llu "
                "replayEpoch=%u",
                ASFW::Audio::Runtime::TxProducerFaultStageName(stage),
                ASFW::Audio::Runtime::TxProducerFaultReasonName(
                    producerReason),
                producerGeneration,
                static_cast<uint32_t>(runtimeReason),
                runtimeGeneration,
                packetIndex,
                startPacketIndex,
                limitPacketIndex,
                preparedCount,
                completionCursor,
                exposeCursor,
                failure.replayProducerCursor,
                failure.replayEpoch);

            if (txControl) {
                txControl->statusWord.store(
                    ASFW::Isoch::IsochTxQueueStatus::kProducerFault,
                    std::memory_order_release);
            }
            ivars.runtime.txActive.store(
                false, std::memory_order_release);
        };

    if (numSlots == 0 || metadataRing == nullptr ||
        ivars.runtime.txSlotProvider.queueControl == nullptr) {
        failProducer(
            ASFW::Audio::Runtime::TxProducerFaultStage::kPreflight,
            ASFW::Audio::Runtime::TxProducerFaultReason::
                kInvalidTransport,
            ASFW::Audio::Runtime::FatalStreamReason::
                InvalidGeometry,
            startPacketIndex);
        return 0;
    }

    // Below this the transport's next refill loads descriptors: a packet here
    // must be committed now, even if its content is not ready.
    const uint64_t mustCommitBefore =
        ivars.runtime.txSlotProvider.queueControl->completionCursor.load(
            std::memory_order_acquire) +
        ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets +
        ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
    const auto requestHeaderlessRecovery = [&]() noexcept {
        const uint32_t lossRun = ivars.runtime.rxReplayLossRun.fetch_add(
            1, std::memory_order_relaxed) + 1;
        auto* current = ivars.runtime.directAudioGraph.control;
        if (lossRun >= 32 && current && ivars.device.audioNub &&
            ivars.runtime.txActive.load(std::memory_order_acquire) &&
            !ivars.runtime.rxReplayRecoveryRequested.exchange(
                true, std::memory_order_acq_rel)) {
            ivars.device.audioNub->RequestTimingRecovery(
                current->rxReplayEpochResets.load(std::memory_order_acquire));
        }
    };

    while (nextPacketToPrepare < limitPacketIndex &&
           preparedCount < maxToPrepare) {
        if (nextPacketToPrepare >= requiredPacketIndex) {
            break;
        }
        // A pass can prepare a whole coverage lead (1008 packets); stop as
        // soon as StopIO has cleared txActive instead of finishing it.
        if (!ivars.runtime.txActive.load(std::memory_order_acquire)) {
            break;
        }

        ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
        timing.disposition =
            ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::NoData;
        const bool bootstrapCadence =
            ivars.runtime.rxReplayAfterBootstrap.load(std::memory_order_acquire) &&
            !allowRecoveredClock;
        // Start with configured cadence; after RX history establishes, the
        // next writable index is the immutable transition boundary.
        timing.replayValid = !bootstrapCadence;
        if (bootstrapCadence) {
            timing.disposition =
                ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::Data;
        }

        ASFW::Audio::BeBoB::MAudioInternalTxTiming::PacketPlan mAudioPlan{};
        if (ivars.runtime.mAudioInternalTxActive.load(std::memory_order_acquire)) {
            if (!ivars.runtime.mAudioInternalTxTiming.PreviewNextPacket(mAudioPlan) ||
                mAudioPlan.sequence != nextPacketToPrepare) {
                failProducer(
                    ASFW::Audio::Runtime::TxProducerFaultStage::kInternalCadence,
                    ASFW::Audio::Runtime::TxProducerFaultReason::kCadencePlanMismatch,
                    ASFW::Audio::Runtime::FatalStreamReason::TxReplayInvalidSyt,
                    nextPacketToPrepare);
                break;
            }
            int64_t packetAnchorTicks = 0;
            if (mAudioPlan.isData &&
                ivars.runtime.txExecutionTimeline.AnchorForPacket(
                    nextPacketToPrepare, packetAnchorTicks)) {
                const uint32_t transmitCycle = static_cast<uint32_t>(
                    (ASFW::Timing::normalizeOffsetDomain(packetAnchorTicks) /
                     ASFW::Timing::kTicksPerCycle) %
                    ASFW::Timing::kCyclesPerSecond);
                timing.replayDataBlocks = mAudioPlan.dataBlocks;
                timing.disposition =
                    ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::Data;
                timing.txClockValid = true;
                timing.nextDataSyt = ASFW::Audio::BeBoB::MAudioInternalTxSyt(
                    mAudioPlan.sytOffsetTicks, transmitCycle,
                    ivars.runtime.mAudioInternalTxTiming.TransferDelayTicks());
            }
        } else if (allowRecoveredClock) {
            int64_t packetAnchorTicks = 0;
            if (!ivars.runtime.txExecutionTimeline.AnchorForPacket(
                    nextPacketToPrepare, packetAnchorTicks)) {
                directControl->txReplayUnderflows.fetch_add(
                    1, std::memory_order_relaxed);
                failProducer(
                    ASFW::Audio::Runtime::TxProducerFaultStage::
                        kExecutionAnchor,
                    ASFW::Audio::Runtime::TxProducerFaultReason::
                        kReplayUnavailable,
                    ASFW::Audio::Runtime::FatalStreamReason::
                        TxReplayUnavailable,
                    nextPacketToPrepare);
                break;
            }
            // The cycle this packet goes out in, derived exactly as the SYT trace below
            // derives outCycle. MOTU stamps each block's SPH relative to it.
            timing.transmitCycle = static_cast<uint32_t>(
                (ASFW::Timing::normalizeOffsetDomain(packetAnchorTicks) /
                 ASFW::Timing::kTicksPerCycle) %
                ASFW::Timing::kCyclesPerSecond);
            timing.transmitCycleValid = true;
            timing.transmitTicks = packetAnchorTicks;

            // A replay stall is transient, not fatal. RX bumps its replay epoch
            // on every rebind/discontinuity (aggregate StartIO/StopIO churn, a
            // packet gap), which invalidates the reader's epoch, and the reader
            // can momentarily outrun the producer. Killing TX here would leave the
            // stream permanently silent -- the timing-loss recovery is health-gated
            // when the device clock is fine (see DiceFamilyAdapter::JudgeRuntimeFault), and even ungated
            // a coordinator restart cannot re-prime TX. Persistent unavailability
            // degrades to silence, which is the correct "nothing to send yet"
            // state, not a stream death.
            if (!ivars.runtime.txReplayReader.IsActive()) {
                (void)ivars.runtime.txReplayReader.Begin(
                    directControl->rxSequenceReplay);
            }
            const bool headerlessReplayMode =
                ivars.runtime.rxReplayAfterBootstrap.load(std::memory_order_acquire) &&
                ivars.runtime.txStreamEngine.StreamConfig().packetFraming ==
                    ASFW::Protocols::Audio::AMDTP::AmdtpStreamConfig::
                        PacketFraming::Headerless;

            ASFW::Audio::Runtime::RxSequenceEntry replay{};
            ASFW::Audio::Runtime::RxSequenceReplayReadDiagnostic replayDiagnostic{};
            bool replayReadable = ivars.runtime.txReplayReader.TryRead(
                directControl->rxSequenceReplay, replay,
                &replayDiagnostic);
            // A reader that fell out of the bounded RX history (kCapacity) is
            // repositionable, not faulted: Begin() re-anchors kReadDelay
            // behind the live producer and the skipped entries only shift
            // NODATA placement, which IEC 61883-6 blocking permits (DBC
            // continuity is packetizer-owned; the SYT offset drifts sub-tick
            // across the skipped span). Frame-cursor alignment must NOT
            // re-arm for this: re-projecting abandons the established
            // host-frame mapping and orphans every host frame behind the new
            // cursor (the all-zero-payload Duet zombie of 2026-07-19).
            if (!replayReadable &&
                replayDiagnostic.failure ==
                    ASFW::Audio::Runtime::RxSequenceReplayReadFailure::
                        kHistoryOverwritten && !headerlessReplayMode) {
                if (ivars.runtime.txReplayReader.Begin(
                        directControl->rxSequenceReplay)) {
                    replayReadable = ivars.runtime.txReplayReader.TryRead(
                        directControl->rxSequenceReplay, replay,
                        &replayDiagnostic);
                }
                ASFW_LOG_RING_ONLY_RL(
                    DirectAudio,
                    "tx-replay-reclamp",
                    1000u,
                    Warning,
                    "[TxReplay] reclamped pkt=%llu cur=%llu prod=%llu ok=%u",
                    nextPacketToPrepare,
                    replayDiagnostic.readerCursor,
                    replayDiagnostic.producerCursor,
                    replayReadable ? 1u : 0u);
            }
            if (replayReadable) {
                ivars.runtime.rxReplayLossRun.store(0, std::memory_order_relaxed);
                directControl->txReplayEntries.fetch_add(
                    1, std::memory_order_relaxed);
                timing.replayDataBlocks = replay.dataBlocks;
            } else if (replayDiagnostic.failure ==
                           ASFW::Audio::Runtime::RxSequenceReplayReadFailure::
                               kAheadOfProducer &&
                       nextPacketToPrepare >= mustCommitBefore) {
                if (headerlessReplayMode) {
                    requestHeaderlessRecovery();
                }
                // Existing CIP replay must stop here: NO-DATA would move its
                // SYT/frame mapping. Headerless RME has no presentation field
                // to corrupt, so bounded skip cycles keep OHCI committed while
                // recovery is queued.
                if (!headerlessReplayMode) {
                    break;
                }
            } else {
                if (headerlessReplayMode) {
                    requestHeaderlessRecovery();
                }
                const int64_t replayDistance =
                    replayDiagnostic.readerCursor >= replayDiagnostic.producerCursor
                        ? static_cast<int64_t>(replayDiagnostic.readerCursor -
                                               replayDiagnostic.producerCursor)
                        : -static_cast<int64_t>(replayDiagnostic.producerCursor -
                                                replayDiagnostic.readerCursor);
                // This is the primary discriminator for a TX silence: it says
                // whether RX had not produced this entry yet, had overwritten it,
                // reset its epoch, or changed the slot while it was being read.
                ASFW_LOG_RING_ONLY_RL(
                    DirectAudio,
                    "tx-replay-read",
                    1000u,
                    Warning,
                    "[TxReplay] fail=%s pkt=%llu cur=%llu prod=%llu d=%lld ep=%u/%u slot=%llu/%u est=%u",
                    ASFW::Audio::Runtime::RxSequenceReplayReadFailureName(
                        replayDiagnostic.failure),
                    nextPacketToPrepare,
                    replayDiagnostic.readerCursor,
                    replayDiagnostic.producerCursor,
                    replayDistance,
                    replayDiagnostic.readerEpoch,
                    replayDiagnostic.replayEpoch,
                    replayDiagnostic.slotSequence,
                    replayDiagnostic.slotEpoch,
                    replayDiagnostic.replayEstablished ? 1u : 0u);
                directControl->txReplayUnderflows.fetch_add(
                    1, std::memory_order_relaxed);
                timing.replayDataBlocks = 0;
                if (replayDiagnostic.failure ==
                    ASFW::Audio::Runtime::RxSequenceReplayReadFailure::
                        kAheadOfProducer) {
                    // Ahead of RX inside the descriptor floor: the hardware
                    // would reach an uncommitted slot, so this one NO-DATA is
                    // forced. It costs one cycle of lag; counted and logged.
                    directControl->txReplayForcedNoData.fetch_add(
                        1, std::memory_order_relaxed);
                    ASFW_LOG_RING_ONLY_RL(
                        DirectAudio,
                        "tx-replay-forced",
                        1000u,
                        Warning,
                        "[TxReplay] forced NO-DATA pkt=%llu mustCommitBefore=%llu cur=%llu prod=%llu",
                        nextPacketToPrepare,
                        mustCommitBefore,
                        replayDiagnostic.readerCursor,
                        replayDiagnostic.producerCursor);
                } else {
                    // Epoch change, establishment loss, or a seqlock miss: the
                    // RX timing domain itself moved. Drop the reader so the
                    // next packet re-Begins on the live epoch and re-arm the
                    // frame-cursor alignment: while stalled we emit NO-DATA
                    // packets, which do NOT advance the content-frame cursor,
                    // so it freezes at its pre-stall frame while CoreAudio
                    // keeps writing. Re-arming makes the first DATA packet
                    // after replay recovers re-project the cursor to the live
                    // frame, closing the gap.
                    // This branch is what arms the [TxAlign] self-heal, and it
                    // was silent: a recurring re-anchor showed up as a ~112 ms
                    // frame-cursor jump with nothing naming the cause. The
                    // reasons are not equivalent - kHistoryOverwritten means the
                    // reader fell behind, which is NOT the timing-domain move
                    // this branch assumes, and re-anchoring would hide it. Name
                    // the reason so the two cases can be told apart.
                    ASFW_LOG_RL(
                        DirectAudio, "tx-replay-rearm", 1000, Warning,
                        "[TxReplayRearm] reason=%{public}s cur=%llu prod=%llu ep=%u/%u "
                        "slot=%llu/%u est=%u",
                        ASFW::Audio::Runtime::RxSequenceReplayReadFailureName(
                            replayDiagnostic.failure),
                        replayDiagnostic.readerCursor,
                        replayDiagnostic.producerCursor,
                        replayDiagnostic.readerEpoch,
                        replayDiagnostic.replayEpoch,
                        replayDiagnostic.slotSequence,
                        replayDiagnostic.slotEpoch,
                        replayDiagnostic.replayEstablished ? 1u : 0u);
                    if (!headerlessReplayMode) {
                        ivars.runtime.txReplayReader.Reset();
                        ivars.runtime.txStreamEngine.ReArmFrameCursorAlignment();
                        if (ivars.runtime.txSecondaryActive) {
                            ivars.runtime.txStreamEngineSecondary
                                .ReArmFrameCursorAlignment();
                        }
                    }
                }
            }

            if (replay.dataBlocks != 0) {
                const bool headerlessReplay =
                    ivars.runtime.rxReplayAfterBootstrap.load(std::memory_order_acquire) &&
                    ivars.runtime.txStreamEngine.StreamConfig().packetFraming ==
                        ASFW::Protocols::Audio::AMDTP::AmdtpStreamConfig::
                            PacketFraming::Headerless;
                // A replayed entry with data blocks but no SYT offset is corruption for
                // an SYT-aware family -- but it is the normal case for MOTU, whose capture
                // side correctly never sets kValidSyt. Linux treats it as an ordinary DATA
                // packet: the SYT goes out as CIP_SYT_NO_INFO and data_blocks is replayed
                // regardless (amdtp-stream.c:1033-1037; the capture cache stores
                // CIP_SYT_NO_INFO for these at :518-521). Faulting here killed transmit
                // 22 ms after StartIO succeeded, so the device started but stayed silent.
                const bool hasReplaySyt =
                    replay.sytOffset !=
                        ASFW::Audio::Runtime::RxSequenceReplayState::kNoInfo &&
                    (replay.flags &
                     ASFW::Audio::Runtime::RxSequenceFlags::kValidSyt) != 0;
                const bool sytUnaware = headerlessReplay ||
                    ivars.runtime.txStreamEngine.IsSytUnaware();
                // kNoInfo is UINT32_MAX; the presentation arithmetic below must never see
                // it. For an SYT-unaware family the packet's cycle timer is the best
                // anchor available, so contribute no sub-cycle offset -- at most one cycle
                // of error, which the framesPerDataPacket alignment absorbs, while the
                // per-block SPH carries the exact timing.
                const uint32_t replaySytOffset = hasReplaySyt ? replay.sytOffset : 0U;
                if (!hasReplaySyt && !sytUnaware) {
                    directControl->txReplayInvalidSyt.fetch_add(
                        1, std::memory_order_relaxed);
                    failProducer(
                        ASFW::Audio::Runtime::TxProducerFaultStage::
                            kReplaySytValidation,
                        ASFW::Audio::Runtime::TxProducerFaultReason::
                                kInvalidReplaySyt,
                        ASFW::Audio::Runtime::FatalStreamReason::
                            TxReplayInvalidSyt,
                        nextPacketToPrepare);
                    break;
                }

                timing.txClockValid = true;
                timing.disposition =
                    ASFW::Protocols::Audio::AMDTP::
                        AmdtpPacketDisposition::Data;
                const uint32_t txDelay =
                    directControl->txTransferDelayTicks.load(
                        std::memory_order_relaxed);
                timing.nextDataSyt =
                    hasReplaySyt
                        ? ASFW::Audio::Runtime::ComputeReplaySytFromTicks(
                              replay.sytOffset, packetAnchorTicks, txDelay)
                        : ASFW::Protocols::Audio::IEC61883::SytFormatter::kNoInfo;

                // [TxSyt]: the stream's first replayed SYT decision, once per
                // stream: the device's SYT (reconstructed from the replayed
                // delay-free offset against its source cycle), that offset,
                // and the transmit SYT re-anchored to our cycle.
                if (hasReplaySyt && !ivars.runtime.txSytLogged) {
                    ivars.runtime.txSytLogged = true;
                    const uint32_t sourceCycle =
                        ASFW::Timing::decodeCycleTimer(replay.sourceCycleTimer).cycle;
                    const auto outCycle = static_cast<uint32_t>(
                        (ASFW::Timing::normalizeOffsetDomain(packetAnchorTicks) /
                         ASFW::Timing::kTicksPerCycle) %
                        ASFW::Timing::kCyclesPerSecond);
                    const auto observedRxSyt = static_cast<uint16_t>(
                        ASFW::Audio::Runtime::ComputeReplaySyt(
                            replay.sytOffset, replay.sourceCycleTimer,
                            directControl->rxTransferDelayTicks.load(
                                std::memory_order_relaxed)));
                    const auto txSyt = static_cast<uint16_t>(timing.nextDataSyt);
                    ASFW_LOG(TxSyt,
                             "obsCyc=%u rxSyt=0x%04x sytOffDelayFree=%u +txDelay=%u outCyc=%u "
                             "=> txSyt=0x%04x (cyc=%u off=0x%03x) pkt=%llu rate=%u rxDbc=%u rxBlocks=%u",
                             sourceCycle, observedRxSyt, replay.sytOffset, txDelay, outCycle,
                             txSyt, (static_cast<uint32_t>(txSyt) >> 12) & 0x0fu,
                             static_cast<uint32_t>(txSyt) & 0x0fffu, nextPacketToPrepare,
                             ivars.runtime.txStreamEngine.StreamConfig().sampleRate,
                             replay.dbc, replay.dataBlocks);
                }

                if (!headerlessReplay) {
                const int64_t sourcePresentationTicks =
                    ASFW::Timing::normalizeOffsetDomain(
                        ASFW::Timing::encodedTstampToOffsets(
                            replay.sourceCycleTimer) +
                        replaySytOffset +
                        directControl
                            ->rxTransferDelayTicks.load(
                                std::memory_order_relaxed));
                const int64_t outputPresentationTicks =
                    ASFW::Timing::normalizeOffsetDomain(
                        packetAnchorTicks +
                        replaySytOffset +
                        directControl
                            ->txTransferDelayTicks.load(
                                std::memory_order_relaxed));
                const int64_t presentationDeltaTicks =
                    ASFW::Timing::extOffsetDiff(
                        outputPresentationTicks,
                        sourcePresentationTicks);
                if (presentationDeltaTicks >= 0) {
                    // ticks -> frames at the live rate. 44.1k has no integer
                    // ticks/sample (24576000/44100 ~= 557.28), so divide the
                    // tick*rate product instead of dividing by a per-sample
                    // constant (the old /512 overshot ~8.8% at 44.1k).
                    const auto& txConfig =
                        ivars.runtime.txStreamEngine.StreamConfig();
                    const uint64_t projectedFrame =
                        ASFW::Audio::DriverKit::ProjectTxFrameCursor(
                            replay.firstAudioFrame,
                            static_cast<uint64_t>(presentationDeltaTicks),
                            txConfig.sampleRate);
                    const uint64_t alignedFrame = projectedFrame;
                    const bool aligned =
                        ivars.runtime.txStreamEngine
                            .AlignFrameCursorOnce(alignedFrame);
                    if (ivars.runtime.txSecondaryActive) {
                        (void)ivars.runtime.txStreamEngineSecondary
                            .AlignFrameCursorOnce(alignedFrame);
                    }
                    // Fires once at stream start, then again each time replay
                    // recovers after a stall re-armed the cursor. A 2nd+ line is
                    // the self-heal closing a deficit that would otherwise be
                    // permanent silence; anomaly-only, so a clean run prints one.
                    if (aligned) {
                        ASFW_LOG_DEBUG(DirectAudio,
                                       "[TxAlign] frame cursor -> %llu (projected=%llu "
                                       "rxFirstFrame=%llu deltaTicks=%lld rate=%u)",
                                       alignedFrame,
                                       projectedFrame,
                                       replay.firstAudioFrame,
                                       static_cast<long long>(presentationDeltaTicks),
                                       txConfig.sampleRate);
                    }
                }
                }
            }
        }

        if (motuSphActive) {
            ApplyMotuSphServoInput(ivars, *directControl);
        }
        ApplyCipSourceId(ivars, *directControl);

        const auto prepareResult =
            ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                nextPacketToPrepare,
                timing);
        if (prepareResult !=
            ASFW::Protocols::Audio::DICE::TxSlotPrepareResult::
                kPrepared) {
            ASFW::Audio::Runtime::TxProducerFaultStage stage =
                ASFW::Audio::Runtime::TxProducerFaultStage::kSlotAcquire;
            ASFW::Audio::Runtime::TxProducerFaultReason producerReason =
                ASFW::Audio::Runtime::TxProducerFaultReason::
                    kSlotUnavailable;
            ASFW::Audio::Runtime::FatalStreamReason runtimeReason =
                ASFW::Audio::Runtime::FatalStreamReason::
                    TxSlotInvariant;

            switch (prepareResult) {
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPacketizerRejected:
                    stage =
                        ASFW::Audio::Runtime::TxProducerFaultStage::
                            kPacketize;
                    producerReason =
                        ASFW::Audio::Runtime::TxProducerFaultReason::
                                kPacketizerRejected;
                    runtimeReason =
                        ASFW::Audio::Runtime::FatalStreamReason::
                            InvalidGeometry;
                    break;
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kSlotPublishFailed:
                    stage =
                        ASFW::Audio::Runtime::TxProducerFaultStage::
                            kSlotPublish;
                    producerReason =
                        ASFW::Audio::Runtime::TxProducerFaultReason::
                                kSlotPublishFailed;
                    break;
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kSlotProviderUnavailable:
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kSlotAcquireFailed:
                    break;
                case ASFW::Protocols::Audio::DICE::
                    TxSlotPrepareResult::kPrepared:
                    break;
            }
            failProducer(
                stage,
                producerReason,
                runtimeReason,
                nextPacketToPrepare);
            break;
        }

        // Shadow the master's per-packet timing on the secondary stream so both
        // device RX streams advance in lockstep (same packetIndex/DBC/SYT/
        // disposition), differing only in payload (channels 17–32). Best-effort:
        // a secondary hiccup must never stall the master (channels 1–16).
        if (ivars.runtime.txSecondaryActive) {
            (void)ivars.runtime.txStreamEngineSecondary.PrepareNextTransmitSlot(
                nextPacketToPrepare, timing);
        }

        const uint32_t slotIdx =
            static_cast<uint32_t>(
                nextPacketToPrepare % numSlots);
        const auto& meta = metadataRing[slotIdx];
        // Ask the packetizer what it emitted; packet size is not a DATA test.
        // M-Audio cadence NO-DATA packets are full-size (CF-labelled blocks).
        const auto* preparedSlot =
            ivars.runtime.txStreamEngine.Timeline().SlotByIndex(
                nextPacketToPrepare);
        const bool emittedData = preparedSlot != nullptr && preparedSlot->isData;
        // The first DATA packets' SPH against the cycle they go out in: the
        // lead the device actually sees (b95 report computed ~2 cycles from
        // the dump and the ring seed; this reads both from the same place).
        if (emittedData && motuSphActive && timing.transmitCycleValid &&
            ivars.runtime.motuStampLogged < 4 && preparedSlot->packetBytes != nullptr &&
            preparedSlot->packetSizeBytes >= 12) {
            ++ivars.runtime.motuStampLogged;
            const uint8_t* b = preparedSlot->packetBytes + 8;
            const uint32_t sph = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                                 (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
            const int64_t sphTicks = static_cast<int64_t>(((sph >> 12) & 0x1fffU) * 3072U + (sph & 0xfffU));
            const int64_t anchorTicks = static_cast<int64_t>(
                ASFW::Timing::normalizeOffsetDomain(timing.transmitTicks) %
                static_cast<int64_t>(ASFW::Timing::kTicksPerSecond));
            int64_t lead = sphTicks - anchorTicks;
            const int64_t second = static_cast<int64_t>(ASFW::Timing::kTicksPerSecond);
            if (lead > second / 2) lead -= second;
            if (lead < -second / 2) lead += second;
            ASFW_LOG_TRACE(DirectAudio,
                           "[MotuTxStamp] pkt=%llu txCycle=%u sph0=%u:%u lead=%lld ticks (%lld.%02lld cycles)",
                           static_cast<unsigned long long>(nextPacketToPrepare), timing.transmitCycle,
                           (sph >> 12) & 0x1fffU, sph & 0xfffU, static_cast<long long>(lead),
                           static_cast<long long>(lead / 3072), static_cast<long long>((lead % 3072 + 3072) % 3072 * 100 / 3072));
        }
        if (ivars.runtime.mAudioInternalTxActive.load(std::memory_order_acquire) &&
            !ivars.runtime.mAudioInternalTxTiming.CommitPacket(
                mAudioPlan, emittedData)) {
            failProducer(
                ASFW::Audio::Runtime::TxProducerFaultStage::kInternalCadence,
                ASFW::Audio::Runtime::TxProducerFaultReason::kCadenceCommitRejected,
                ASFW::Audio::Runtime::FatalStreamReason::TxReplayInvalidSyt,
                nextPacketToPrepare);
            break;
        }
        const bool headerless = ivars.runtime.txStreamEngine.StreamConfig().packetFraming ==
            ASFW::Protocols::Audio::AMDTP::AmdtpStreamConfig::PacketFraming::Headerless;
        if (emittedData) {
            directControl->counters.txDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            if (!headerless) {
                directControl->counters.txValidSytPackets.fetch_add(
                    1, std::memory_order_relaxed);
            }
        } else if (!headerless && meta.payloadLength == 0) {
            directControl->counters.txEmptyPackets.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            directControl->counters.txNoDataPackets.fetch_add(
                1, std::memory_order_relaxed);
            if (!headerless) {
                directControl->counters.txSytFfffPackets.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        directControl->counters.txPackets.fetch_add(
            1, std::memory_order_relaxed);

        ++nextPacketToPrepare;
        ++preparedCount;
        if (ivars.runtime.rxReplayAfterBootstrap.load(std::memory_order_acquire) &&
            ivars.runtime.txStreamEngine.StreamConfig().packetFraming ==
                ASFW::Protocols::Audio::AMDTP::AmdtpStreamConfig::
                    PacketFraming::Headerless &&
            ivars.runtime.rxReplayRecoveryRequested.load(std::memory_order_acquire) &&
            preparedCount >= 32) {
            break;
        }
    }

    return preparedCount;
}

void PrefillTxRingBeforeStart(ASFWAudioDriver_IVars& ivars) noexcept {
    const uint32_t numSlots = ivars.runtime.txSlotProvider.numSlots;
    auto* metadataRing = ivars.runtime.txSlotProvider.metadataRing;
    if (numSlots == 0 || metadataRing == nullptr) {
        return;
    }
    if (const auto* control = ivars.runtime.directAudioGraph.control; control != nullptr) {
        ApplyCipSourceId(ivars, *control);
    }

    // Commit one complete shared-ring lap before IT RUN. The transport's arm
    // contract validates this exact prefill so that a delayed first producer
    // action cannot expose an uncommitted slot to IT DMA. Steady state still
    // targets completion + kTxPreparationLeadPackets.
    ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
    timing.disposition =
        ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::NoData;
    timing.replayValid = !ivars.runtime.rxReplayAfterBootstrap.load(
        std::memory_order_acquire);
    if (!timing.replayValid) {
        timing.disposition =
            ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::Data;
    }
    timing.txClockValid = false;
    // A MOTU stream primes its lap with skip cycles, as Linux primes an OUT
    // stream (amdtp_stream_start queues header_length == payload_length == 0
    // packets, which queue_out_packet turns into skips), so nothing reaches
    // the wire before the first real packet. Until b107 the lap was 1512
    // header-only CIP packets carrying DBC 0 (189 ms); the 828mkII fresh from
    // a power cycle aborted its own stream on the first data packet that
    // followed, every start since b74, and never after a skip-cycle lap
    // (b104 wire capture, b107 runs). Other families keep their recorded
    // start sequences.
    timing.skipCycle = ivars.runtime.motuSphActive.load(std::memory_order_relaxed) &&
                       timing.disposition == ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::NoData;

    uint32_t prepared = 0;
    for (uint64_t packetIndex = 0;
         packetIndex < numSlots;
         ++packetIndex) {
        ASFW::Audio::BeBoB::MAudioInternalTxTiming::PacketPlan mAudioPlan{};
        if (ivars.runtime.mAudioInternalTxActive.load(std::memory_order_acquire) &&
            (!ivars.runtime.mAudioInternalTxTiming.PreviewNextPacket(mAudioPlan) ||
             mAudioPlan.sequence != packetIndex)) {
            break;
        }
        if (ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                packetIndex, timing) !=
            ASFW::Protocols::Audio::DICE::TxSlotPrepareResult::
                kPrepared) {
            break;
        }
        // Seed the secondary ring in lockstep with the same NO-DATA packets.
        if (ivars.runtime.txSecondaryActive) {
            (void)ivars.runtime.txStreamEngineSecondary.PrepareNextTransmitSlot(
                packetIndex, timing);
        }
        if (ivars.runtime.mAudioInternalTxActive.load(std::memory_order_acquire) &&
            !ivars.runtime.mAudioInternalTxTiming.CommitPacket(mAudioPlan, false)) {
            break;
        }
        ++prepared;
    }

    ASFW_LOG_DEBUG(DirectAudio,
                   "ADK DBG TX prefill seeded %u/%u committed %{public}s before isoch start (steadyLead=%u)",
                   prepared,
                   numSlots,
                   timing.skipCycle ? "skip cycles (nothing on the wire)"
                              : (timing.disposition == ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition::NoData
                                     ? "NO-DATA packets" : "cadence packets"),
                   ASFW::IsochTransport::AudioTimingGeometry::
                 kTxPreparationLeadPackets);
}

PrimaryTxArmResult ArmPrimaryTxProducer(
    ASFWAudioDriver_IVars& ivars,
    const ASFW::Isoch::Audio::IAudioStreamProfile& profile,
    const ASFW::Isoch::Audio::AudioStreamConfig& txConfig,
    const PrimaryTxQueueMemory& memory) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    if (control == nullptr || memory.queueControl == nullptr) {
        return {kIOReturnNotReady, "BindPrimaryTxQueue"};
    }

    // Clear stale runtime cursors before prefill: the shared slab can be
    // reused across StartIO/StopIO probes (CoreAudio re-probes on a
    // sample-rate change), and a carried-over committed cursor fails the IT
    // prime ("committed prefill > slots").
    memory.queueControl->ResetProducerForStart();

    ivars.runtime.motuStampLogged = 0;
    ivars.runtime.txCadenceLastData = 0;
    ivars.runtime.txCadenceLastNoData = 0;
    ivars.runtime.txCadenceLastEmpty = 0;
    ivars.runtime.txSlotProvider.payloadBase = memory.payloadBase;
    ivars.runtime.txSlotProvider.metadataRing = memory.metadataRing;
    ivars.runtime.txSlotProvider.queueControl = memory.queueControl;
    ivars.runtime.txSlotProvider.numSlots = memory.numSlots;
    ivars.runtime.txSlotProvider.slotStrideBytes = memory.slotStrideBytes;
    // Nothing observes release unless this start binds an observer below.
    ivars.runtime.txSlotProvider.releaseObserver = nullptr;
    ivars.runtime.motuTxPhaseTrace.Unbind();

    ivars.runtime.txExecutionTimeline.queueControl = memory.queueControl;

    // A device's own slot order (the Phase 88 is planar) arrives in txConfig
    // from its discovered graph; Configure prefers it over the profile's map.
    if (!ivars.runtime.txStreamEngine.Configure(profile, txConfig)) {
        ASFW_LOG_ERROR(Audio, "ASFWAudioDevice: txStreamEngine Configure failed");
        return {kIOReturnError, "ConfigureTxStreamEngine"};
    }
    ivars.runtime.txStreamEngine.SetTimingLossCallback({});
    ivars.runtime.motuSphActive.store(false, std::memory_order_relaxed);
    const auto txPolicy = profile.TxStreamPolicy();
    if (txPolicy.hostToDevicePcmEncoding == ASFW::Encoding::AudioWireFormat::kMotuPacked) {
        // Every MOTU model is timed by the SPH regulator, with the model's
        // lead and loop mode; a rate it has no configuration for cannot be
        // timed at all.
        const uint32_t leadTicks = txPolicy.motuPresentationLeadTicks;
        const auto setpoint = txPolicy.motuHoldAbsolutePhase
            ? ::ASFW::Encoding::Motu::AbsolutePhaseSetpointTicksAtRate(
                  txPolicy.motuAbsolutePhaseSetpointTicks48k, txConfig.sampleRate)
            : std::nullopt;
        ASFW_LOG_DEBUG(DirectAudio, "[MotuSphConfig] lead=%u ticks (%u.%02u cycles) absolutePhase=%{public}s rate=%u",
                       leadTicks, leadTicks / 3072U, (leadTicks % 3072U) * 100U / 3072U,
                       setpoint.has_value() ? "held" : "relative", txConfig.sampleRate);
        if (!ivars.runtime.motuTxTimingStamper.Configure(
                txConfig.dbs, txConfig.sampleRate, leadTicks, setpoint)) {
            ASFW_LOG(Audio, "ASFWAudioDevice: MOTU SPH regulator has no configuration for rate=%u",
                     txConfig.sampleRate);
            return {kIOReturnUnsupported, "ConfigureMotuTxTiming"};
        }
        ivars.runtime.motuPayloadWriter.Configure(
            ::ASFW::Encoding::Motu::MotuPayloadStreamConfig{
                .pcmChunks = txConfig.pcmChannels,
                .sourceChannelOffset = txConfig.sourceChannelOffset,
                .ports = txPolicy.motuPlaybackPorts,
                .pcmByteOffset = txConfig.motuPcmByteOffset});
        ivars.runtime.motuPayloadWriter.BindTimeline(&ivars.runtime.txStreamEngine.Timeline());
        ivars.runtime.txStreamEngine.SetPayloadWriter(&ivars.runtime.motuPayloadWriter);

        ivars.runtime.txStreamEngine.SetTimingLossCallback([state = &ivars] {
            if (!state->runtime.isRunning.load(std::memory_order_acquire)) return false;
            auto* currentControl = state->runtime.directAudioGraph.control;
            auto* nub = state->device.audioNub;
            if (!nub || !currentControl) return false;
            nub->RequestTimingRecovery(
                currentControl->rxReplayEpochResets.load(std::memory_order_acquire));
            return true;
        });
        // Muted from the start until the regulator locks; the servo hook in
        // PrepareTransmitSlots keeps the writer on the regulator's gate.
        ivars.runtime.motuPayloadWriter.SetPcmMuted(
            ivars.runtime.motuTxTimingStamper.IsOutputMuted());
        ivars.runtime.txStreamEngine.BindTimingStamper(&ivars.runtime.motuTxTimingStamper);
        // The transmit half of the absolute-phase measurement, where its
        // cadence geometry holds: every released DATA packet's SPH, one
        // cadence period at a time, to the RX timing observer.
        if (ASFW::Audio::Runtime::MotuRelPhaseGeometryHolds(txConfig.sampleRate)) {
            ivars.runtime.motuTxPhaseTrace.Bind(
                control, memory.queueControl,
                ivars.runtime.motuTxTimingStamper.PresentationLeadTicks());
            ivars.runtime.txSlotProvider.releaseObserver = &ivars.runtime.motuTxPhaseTrace;
        }
        ivars.runtime.motuSphAcquireHostTicks = mach_absolute_time();
        ivars.runtime.motuSphLockPending = true;
        ivars.runtime.motuSphActive.store(true, std::memory_order_relaxed);
    }
    ivars.runtime.txStreamEngine.BindSlotProvider(&ivars.runtime.txSlotProvider);
    ivars.runtime.txStreamEngine.ResetForStart(0, 0);
    ivars.runtime.txPlacementLogged = false;
    ivars.runtime.txSytLogged = false;
    ivars.runtime.txFilledFrameEnd = 0;
    ivars.runtime.txClearedFrameEnd = 0;
    // CoreAudio's output ring still holds the previous session's output, and
    // the fill can meet a frame before CoreAudio writes it in this session
    // (b146: a cold start replayed about 170 ms of it). Silence it before IO.
    ClearPlaybackRing(ivars);
    // Peaks are per interval of one stream: the previous start's last
    // interval must not open this start's [TxContent] line.
    (void)ivars.runtime.motuPayloadWriter.TakePeakWrittenQ24();
    (void)ivars.runtime.txStreamEngine.TakePeakWrittenQ24(nullptr);
    // The interval minimum belongs to one stream: drop the previous one's.
    (void)ivars.runtime.txStreamEngine.TakeMinFinalityMarginPackets();
    (void)ivars.runtime.motuPayloadWriter.TakeMinFinalityMarginPackets();
    ivars.runtime.txMissedFinalityAtStart =
        ivars.runtime.txStreamEngine.PayloadWriterCounters().framesMissedFinality.load(
            std::memory_order_relaxed) +
        ivars.runtime.motuPayloadWriter.Counters().framesMissedFinality.load(
            std::memory_order_relaxed);
    ivars.runtime.glitchReportedMissed = 0;
    ivars.runtime.glitchReportedStarved = 0;
    ivars.runtime.glitchReportedAhead = 0;
    ivars.runtime.ioOpTraceCapture.Reset();
    ivars.runtime.glitchReportedPulls = control->rxPull.pulls.load(std::memory_order_relaxed);
    ivars.runtime.glitchReportedPullBusy = control->rxPull.busy.load(std::memory_order_relaxed);
    ivars.runtime.glitchLastReportTicks = 0;
    ivars.runtime.glitchReportedPullCut = control->rxPull.cut.load(std::memory_order_relaxed);
    ivars.runtime.glitchFillMaxTicks.store(0, std::memory_order_relaxed);
    ivars.runtime.glitchEvents.Reset();
    ivars.runtime.ioTiming.Reset(
        mach_absolute_time(),
        ASFW::Timing::nanosToHostTicks(ASFW::Audio::Runtime::kSlowIoOpNanos),
        ASFW::Timing::nanosToHostTicks(ASFW::Audio::Runtime::kIoOpFollowMaxNanos),
        control->rxPull.waited.load(std::memory_order_relaxed),
        control->rxPull.busy.load(std::memory_order_relaxed));
    ivars.runtime.lastBeginReadFrame.store(0, std::memory_order_relaxed);
    ivars.runtime.lastBeginReadTicks.store(0, std::memory_order_relaxed);
    ivars.runtime.txReplayReader.Reset();
    ivars.runtime.rxReplayLossRun.store(0, std::memory_order_relaxed);
    ivars.runtime.rxReplayRecoveryRequested.store(false, std::memory_order_relaxed);

    // The resolved geometry is the only rate/timing source here; there is no
    // 48 kHz fallback (a graph that failed to resolve never reaches StartIO).
    const auto& timing = ivars.device.timing;
    const uint32_t timingRateHz = timing.sampleRateHz;
    if (ivars.runtime.mAudioInternalTxActive.load(
            std::memory_order_acquire)) {
        ++ivars.runtime.mAudioTxClockStartEpoch;
        if (ivars.runtime.mAudioTxClockStartEpoch == 0) {
            ++ivars.runtime.mAudioTxClockStartEpoch;
        }
        ivars.runtime.txCompletionStampCursor = 0;
        ivars.runtime.mAudioTxCorrelationUnwrap = {};
        if (!ivars.runtime.mAudioTxClockBridge.Arm(
                control->hardwareTimeline,
                ivars.runtime.mAudioTxClockStartEpoch,
                timingRateHz,
                timing.zeroTimestampPeriodFrames,
                ivars.runtime.mAudioInternalTxTiming.
                    TransferDelayTicks())) {
            return {kIOReturnUnsupported, "MAudioTxClockBridge"};
        }
        // Once per start, the counterpart of the Receive line in
        // SelectTxClockDomain: which clock owns this start's timeline.
        ASFW_LOG(DirectAudio, "[Zts] epoch=%llu source=transmit reason=start-io rate=%u",
                 ivars.runtime.mAudioTxClockBridge.Epoch(), timingRateHz);
    }
    control->rxTransferDelayTicks.store(timing.rxTransferDelayTicks,
                                        std::memory_order_relaxed);
    control->txTransferDelayTicks.store(timing.txTransferDelayTicks,
                                        std::memory_order_relaxed);
    return {};
}

bool MeasureTxPlacement(ASFWAudioDriver_IVars& ivars, TxPlacementSample& out) noexcept {
    const auto* queue = ivars.runtime.txSlotProvider.queueControl;
    const auto* control = ivars.runtime.directAudioGraph.control;
    if (queue == nullptr || control == nullptr) {
        return false;
    }
    const uint64_t stamps = queue->completionStampCount.load(std::memory_order_acquire);
    uint64_t packet = 0;
    uint32_t stamp = 0;
    if (stamps == 0 || !queue->ReadCompletionStamp(stamps - 1, packet, stamp)) {
        return false;
    }

    // This runs on the preparation queue, which also writes the timeline, so
    // the slot is read without the RT-side seqlock.
    const auto* slot =
        ivars.runtime.txStreamEngine.Timeline().SlotByIndex(static_cast<uint32_t>(packet));
    if (slot == nullptr || slot->packetIndex != packet ||
        !slot->isData || slot->framesInPacket == 0) {
        return false;
    }

    // The packet's transmit cycle, as host time, through the transport's
    // cycle-timer/host-time pair from the same refill.
    ASFW::Isoch::IsochTxClockPairSample pair{};
    if (!queue->clockPair.TryRead(pair) || pair.hostTimeMid == 0) {
        return false;
    }
    const int64_t busDelta = ASFW::Timing::extOffsetDiff(
        ASFW::Timing::encodedTstampToOffsets(stamp),
        ASFW::Timing::encodedTstampToOffsets(pair.cycleTimer32));
    const uint64_t busMagnitude =
        static_cast<uint64_t>(busDelta < 0 ? -busDelta : busDelta);
    const uint64_t busHost = ASFW::Timing::nanosToHostTicks(
        busMagnitude * 1'000'000'000ULL / ASFW::Timing::kTicksPerSecond);
    const uint64_t txHost =
        busDelta >= 0 ? pair.hostTimeMid + busHost : pair.hostTimeMid - busHost;

    // The HAL's sample time at that host time, from the one anchor CoreAudio
    // is fed (Epic 4: HostClockAnchor is the ZTS projection).
    ASFW::Audio::Runtime::HostClockAnchorSample anchor{};
    const uint32_t rate = ivars.runtime.txStreamEngine.StreamConfig().sampleRate;
    if (!control->hostClockAnchor.TryReadLatest(0, anchor) || anchor.hostTicks == 0 ||
        rate == 0) {
        return false;
    }
    const bool after = txHost >= anchor.hostTicks;
    const uint64_t hostNanos = ASFW::Timing::hostTicksToNanos(
        after ? txHost - anchor.hostTicks : anchor.hostTicks - txHost);
    const auto frames = static_cast<int64_t>(
        static_cast<__uint128_t>(hostNanos) * rate / 1'000'000'000ULL);

    out.packetIndex = packet;
    out.firstAudioFrame = slot->firstAudioFrame;
    out.halSampleTime = static_cast<int64_t>(anchor.sampleFrame) + (after ? frames : -frames);
    out.offsetFrames = static_cast<int64_t>(slot->firstAudioFrame) - out.halSampleTime;
    return true;
}

} // namespace ASFW::Audio::DriverKit

void IMPL(ASFWAudioDriver, TxPreparationReady)
{
    (void)action;
    (void)generation;
    if (!ivars ||
        !ivars->runtime.txActive.load(
            std::memory_order_acquire)) {
        return;
    }

    auto* txControl = ivars->runtime.txSlotProvider.queueControl;
    const uint32_t numSlots = ivars->runtime.txSlotProvider.numSlots;
    if (!txControl || numSlots == 0) {
        return;
    }

    const uint64_t requested =
        txControl->refillRequestGeneration.load(
            std::memory_order_acquire);
    const uint64_t refillHandled =
        txControl->refillHandledGeneration.load(
            std::memory_order_acquire);
    const bool hardwareWakePending = requested != refillHandled;

    if (hardwareWakePending && ivars->runtime.mAudioInternalTxActive.load(
                                   std::memory_order_acquire)) {
        ASFW::Audio::DriverKit::ObserveMAudioTxClock(*ivars, requested);
    }

    const uint64_t completionCursor =
        txControl->completionCursor.load(std::memory_order_acquire);
    const uint64_t exposeCursor =
        txControl->committedEnd.load(std::memory_order_acquire);
    const uint64_t packetCoverageTarget =
        completionCursor +
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxCoverageLeadPackets;
    const uint64_t packetLimitTarget =
        completionCursor +
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxPreparationLeadPackets;

    auto* directControl = ivars->runtime.directAudioGraph.control;
    const bool replayEstablished =
        directControl && directControl->rxSequenceReplay.IsEstablished();
    const uint64_t audioRequested = directControl
        ? directControl->txPreparationRequests.RequestedGeneration()
        : 0;
    const uint64_t preparationBegin = mach_absolute_time();
    const uint64_t hardwareRequestedAt = txControl->refillRequestHostTicks.load(std::memory_order_relaxed);
    const uint64_t queueDelayUs = hardwareWakePending && preparationBegin >= hardwareRequestedAt
        ? ASFW::Timing::hostTicksToNanos(preparationBegin - hardwareRequestedAt) / 1000 : 0;
    if (queueDelayUs >= 5000) {
        ASFW_LOG(DirectAudio, "[TxPrepStall] phase=begin queue=TxPreparation generation=%llu queueDelayUs=%llu completion=%llu committed=%llu clientFrame=%llu clientHost=%llu",
            requested, queueDelayUs, completionCursor, exposeCursor,
            directControl ? directControl->client.outputClientWriteEndFrame.load(std::memory_order_acquire) : 0,
            directControl ? directControl->client.outputWriteEndHostTicks.load(std::memory_order_relaxed) : 0);
    }
    const uint32_t slotsPrepared =
        ASFW::Audio::DriverKit::PrepareTransmitSlots(
            *ivars,
            exposeCursor,
            packetCoverageTarget,
            packetLimitTarget,
            ASFW::IsochTransport::AudioTimingGeometry::
                kTxPreparationLeadPackets,
            replayEstablished);

    const uint64_t preparationUs = ASFW::Timing::hostTicksToNanos(mach_absolute_time() - preparationBegin) / 1000;
    if (queueDelayUs >= 5000 || preparationUs >= 5000) {
        ASFW_LOG(DirectAudio, "[TxPrepStall] phase=end queue=TxPreparation generation=%llu queueDelayUs=%llu preparationUs=%llu prepared=%u clientFrame=%llu clientHost=%llu",
            requested, queueDelayUs, preparationUs, slotsPrepared,
            directControl ? directControl->client.outputClientWriteEndFrame.load(std::memory_order_acquire) : 0,
            directControl ? directControl->client.outputWriteEndHostTicks.load(std::memory_order_relaxed) : 0);
    }

    // [TxPrepRange] Refill-coverage instrumentation. Answers the decisive
    // question: did the producer's range reach `target` this wake, or stop
    // short and leave a hole the IT refill ISR will later trip on? The producer
    // loop is linear in absolute packet index, so `prepareUntil` is exactly
    // `base + slotsPrepared`.
    {
        const uint64_t prepareBaseAbs = exposeCursor;
        const uint64_t prepareUntilAbs = exposeCursor + slotsPrepared;
        // A pass may end short of the coverage target on purpose: it stops
        // when RX replay is not there yet (T4). Only a pass that ends below
        // the descriptor floor leaves a hole the refill will trip on.
        const uint64_t descriptorFloor =
            completionCursor +
            ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets +
            ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
        const bool stoppedShort = prepareUntilAbs < descriptorFloor;
        const uint64_t committedMargin =
            prepareUntilAbs > completionCursor
                ? prepareUntilAbs - completionCursor
                : 0;
        // Anomaly-only: log a wake that left a hole below the descriptor
        // floor (it precedes an IT FATAL). The periodic [TxPrep] summary
        // remains the liveness/margin heartbeat.
        if (stoppedShort) {
            ASFW_LOG_RING_ONLY_RL(
                DirectAudio,
                "tx-prep-range",
                0u,
                Warning,
                "[TxPrepRange] short=1 ret=%llu base=%llu until=%llu cov=%llu lim=%llu n=%u margin=%llu",
                completionCursor,
                prepareBaseAbs,
                prepareUntilAbs,
                packetCoverageTarget,
                packetLimitTarget,
                slotsPrepared,
                committedMargin);
        }
    }

    bool scheduleAudioFollowUp = false;
    if (directControl) {
        const uint64_t now = mach_absolute_time();
        const uint64_t requestedAt =
            hardwareWakePending
                ? txControl->refillRequestHostTicks.load(
                      std::memory_order_relaxed)
                : now;
        const uint64_t latency =
            now >= requestedAt ? now - requestedAt : 0;
        const uint64_t latencyNanos =
            ASFW::Timing::hostTicksToNanos(latency);
        if (hardwareWakePending) {
            directControl->txLastPreparationLatencyTicks.store(
                latency, std::memory_order_relaxed);
            directControl->txPreparationLatencySamples.fetch_add(
                1, std::memory_order_relaxed);
            using Geometry = ASFW::IsochTransport::AudioTimingGeometry;
            if (latencyNanos <= Geometry::kTxPreparationLatency750Us *
                                    Geometry::kNanosecondsPerMicrosecond) {
                directControl->txPreparationAtMost750Us.fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (latencyNanos >= Geometry::kTxPreparationLatency1500Us *
                                    Geometry::kNanosecondsPerMicrosecond) {
                directControl->txPreparationAtLeast1500Us.fetch_add(
                    1, std::memory_order_relaxed);
            }
            uint64_t previousMax =
                directControl->txMaxPreparationLatencyTicks.load(
                    std::memory_order_relaxed);
            while (latency > previousMax &&
                   !directControl->txMaxPreparationLatencyTicks
                        .compare_exchange_weak(
                            previousMax,
                            latency,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
            }
            uint64_t previousIntervalMax =
                directControl->txIntervalPreparationLatencyMaxTicks.load(
                    std::memory_order_relaxed);
            while (latency > previousIntervalMax &&
                   !directControl->txIntervalPreparationLatencyMaxTicks
                        .compare_exchange_weak(
                            previousIntervalMax,
                            latency,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
            }

            const size_t latencyBucket =
                latencyNanos < Geometry::kTxPreparationLatency250Us *
                                   Geometry::kNanosecondsPerMicrosecond
                    ? 0
                    : latencyNanos < Geometry::kTxPreparationLatency500Us *
                                         Geometry::kNanosecondsPerMicrosecond
                          ? 1
                          : latencyNanos < Geometry::kTxPreparationLatency750Us *
                                                Geometry::kNanosecondsPerMicrosecond
                                ? 2
                                : latencyNanos < Geometry::kTxPreparationLatency1000Us *
                                                       Geometry::kNanosecondsPerMicrosecond
                                      ? 3
                                      : latencyNanos < Geometry::kTxPreparationLatency1500Us *
                                                             Geometry::kNanosecondsPerMicrosecond
                                            ? 4
                                            : 5;
            directControl->txIntervalPreparationLatencyHistogram[latencyBucket]
                .fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t distance =
            packetLimitTarget > exposeCursor
                ? packetLimitTarget - exposeCursor
                : 0;
        const uint32_t boundedDistance =
            distance > UINT32_MAX
                ? UINT32_MAX
                : static_cast<uint32_t>(distance);
        uint32_t previousMin =
            directControl->txMinimumPreparationDistance.load(
                std::memory_order_relaxed);
        while (boundedDistance < previousMin &&
               !directControl->txMinimumPreparationDistance
                    .compare_exchange_weak(
                        previousMin,
                        boundedDistance,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
        }
        const uint64_t committedMargin =
            exposeCursor > completionCursor
                ? exposeCursor - completionCursor
                : 0;
        const uint32_t boundedMargin =
            committedMargin > UINT32_MAX
                ? UINT32_MAX
                : static_cast<uint32_t>(committedMargin);
        directControl->txCurrentCommittedMarginPackets.store(
            boundedMargin, std::memory_order_relaxed);
        const uint32_t committedMarginFloorBefore =
            directControl->txMinimumCommittedMarginPackets.load(
                std::memory_order_relaxed);
        uint32_t previousMargin = committedMarginFloorBefore;
        while (boundedMargin < previousMargin &&
               !directControl->txMinimumCommittedMarginPackets
                    .compare_exchange_weak(
                        previousMargin,
                        boundedMargin,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
        }
        uint32_t previousIntervalMarginMin =
            directControl->txIntervalCommittedMarginMinPackets.load(
                std::memory_order_relaxed);
        while (boundedMargin < previousIntervalMarginMin &&
               !directControl->txIntervalCommittedMarginMinPackets
                    .compare_exchange_weak(
                        previousIntervalMarginMin,
                        boundedMargin,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
        }
        uint32_t previousIntervalMarginMax =
            directControl->txIntervalCommittedMarginMaxPackets.load(
                std::memory_order_relaxed);
        while (boundedMargin > previousIntervalMarginMax &&
               !directControl->txIntervalCommittedMarginMaxPackets
                    .compare_exchange_weak(
                        previousIntervalMarginMax,
                        boundedMargin,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
        }
        using Geometry = ASFW::IsochTransport::AudioTimingGeometry;
        const size_t marginBucket =
            boundedMargin < Geometry::kTxCommittedMargin2xFloorPackets
                ? 0
                : boundedMargin < Geometry::kTxCommittedMargin4xFloorPackets
                      ? 1
                      : boundedMargin < Geometry::kTxCommittedMargin8xFloorPackets
                            ? 2
                            : boundedMargin < Geometry::kTxCommittedMargin16xFloorPackets
                                  ? 3
                                  : 4;
        directControl->txIntervalCommittedMarginHistogram[marginBucket]
            .fetch_add(1, std::memory_order_relaxed);

        // Before the heartbeat below takes the interval maxima it reports.
        ASFW::Audio::DriverKit::ReportTransportGlitches(*ivars, *directControl, now);
        ASFW::Audio::DriverKit::ReportIoTiming(*ivars, *directControl, now);

        // [TxPrep] Surface the cross-queue preparation health to the log. The
        // refill ISR trips kUnderrunFatal once committedMargin falls to the
        // hardware-owned ring depth, so emit on every new committed-margin low,
        // on every wake beyond the 1.5 ms early-warning threshold, and on a
        // coarse heartbeat. The actual geometry budget is encoded in
        // kTxPreparationSlackPackets. See documentation/ZTS_AND_SYT.md §13.
        const uint32_t minCommittedMargin =
            directControl->txMinimumCommittedMarginPackets.load(
                std::memory_order_relaxed);
        const uint64_t maxLatencyNanos = ASFW::Timing::hostTicksToNanos(
            directControl->txMaxPreparationLatencyTicks.load(
                std::memory_order_relaxed));
        const uint64_t wakeSamples =
            directControl->txPreparationLatencySamples.load(
                std::memory_order_relaxed);
        constexpr uint32_t kCommittedMarginDangerPackets =
            ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets;
        const bool newCommittedMarginLow =
            boundedMargin < committedMarginFloorBefore;

        // Wall-clock heartbeat. A wake-count trigger is rate-dependent: the
        // same divisor emits ~1.3 lines/s at 48 kHz and 2-4x that at 96/192 kHz,
        // where ring retention matters most. Both anomaly triggers below are
        // independent of this, so pacing costs no fault coverage.
        constexpr uint64_t kHeartbeatIntervalNanos = 5'000'000'000ULL;
        const uint64_t lastHeartbeatTicks =
            directControl->txHeartbeatLastHostTicks.load(
                std::memory_order_relaxed);
        const bool heartbeatDue =
            lastHeartbeatTicks == 0 || now <= lastHeartbeatTicks ||
            ASFW::Timing::hostTicksToNanos(now - lastHeartbeatTicks) >=
                kHeartbeatIntervalNanos;

        // A slow wake is counted (late1500), not logged on its own: since T4
        // the transport margin is the coverage target (~16 ms), far above the
        // 1.5 ms wake budget, and the fill's exposure to slow wakes shows up
        // as missedFinality in the heartbeat.
        // A new margin low is an anomaly only near the descriptor floor; the
        // drain from the prefill down to the coverage target at start-up is
        // a sequence of new lows by design.
        const bool nearFloor =
            boundedMargin <= ASFW::IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets +
                                 ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
        if ((newCommittedMarginLow && nearFloor) || heartbeatDue) {
            // Anomaly emissions intentionally close an interval early. This
            // keeps every retained [TxPrep] line self-contained and leaves the
            // normal healthy interval wall-clock paced at five seconds.
            const uint32_t intervalMarginMin =
                directControl->txIntervalCommittedMarginMinPackets.exchange(
                    UINT32_MAX, std::memory_order_relaxed);
            const uint32_t intervalMarginMax =
                directControl->txIntervalCommittedMarginMaxPackets.exchange(
                    0, std::memory_order_relaxed);
            const uint64_t intervalLatencyMaxNanos =
                ASFW::Timing::hostTicksToNanos(
                    directControl->txIntervalPreparationLatencyMaxTicks.exchange(
                        0, std::memory_order_relaxed));
            const uint64_t latencyBucket0 =
                directControl->txIntervalPreparationLatencyHistogram[0].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket1 =
                directControl->txIntervalPreparationLatencyHistogram[1].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket2 =
                directControl->txIntervalPreparationLatencyHistogram[2].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket3 =
                directControl->txIntervalPreparationLatencyHistogram[3].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket4 =
                directControl->txIntervalPreparationLatencyHistogram[4].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t latencyBucket5 =
                directControl->txIntervalPreparationLatencyHistogram[5].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket0 =
                directControl->txIntervalCommittedMarginHistogram[0].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket1 =
                directControl->txIntervalCommittedMarginHistogram[1].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket2 =
                directControl->txIntervalCommittedMarginHistogram[2].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket3 =
                directControl->txIntervalCommittedMarginHistogram[3].exchange(
                    0, std::memory_order_relaxed);
            const uint64_t marginBucket4 =
                directControl->txIntervalCommittedMarginHistogram[4].exchange(
                    0, std::memory_order_relaxed);
            // Publish a stable copy for the read-only user-client snapshot.
            // No control-plane caller receives directControl itself.
            // The interval opened at the previous emission (0 = unknown:
            // first emission after a reset).
            const uint64_t intervalDurationTicks =
                lastHeartbeatTicks != 0 && now > lastHeartbeatTicks
                    ? now - lastHeartbeatTicks
                    : 0;
            ASFW::Audio::Runtime::SeqlockWriteBegin(
                directControl->txCompletedIntervalSequence);
            directControl->txCompletedIntervalDurationTicks.store(
                intervalDurationTicks, std::memory_order_relaxed);
            directControl->txCompletedIntervalEndHostTicks.store(
                now, std::memory_order_relaxed);
            directControl->txCompletedIntervalMarginMinPackets.store(
                intervalMarginMin, std::memory_order_relaxed);
            directControl->txCompletedIntervalMarginMaxPackets.store(
                intervalMarginMax, std::memory_order_relaxed);
            directControl->txCompletedIntervalPreparationLatencyMaxTicks.store(
                ASFW::Timing::nanosToHostTicks(intervalLatencyMaxNanos),
                std::memory_order_relaxed);
            const uint64_t latencyBuckets[] = {
                latencyBucket0, latencyBucket1, latencyBucket2,
                latencyBucket3, latencyBucket4, latencyBucket5,
            };
            for (size_t index = 0; index < std::size(latencyBuckets); ++index) {
                directControl->txCompletedIntervalPreparationLatencyHistogram[index].store(
                    latencyBuckets[index], std::memory_order_relaxed);
            }
            const uint64_t marginBuckets[] = {
                marginBucket0, marginBucket1, marginBucket2,
                marginBucket3, marginBucket4,
            };
            for (size_t index = 0; index < std::size(marginBuckets); ++index) {
                directControl->txCompletedIntervalCommittedMarginHistogram[index].store(
                    marginBuckets[index], std::memory_order_relaxed);
            }
            ASFW::Audio::Runtime::SeqlockWriteEnd(
                directControl->txCompletedIntervalSequence);
            // Close the RX interval on the same boundary. If the receive path
            // is closing it concurrently this is skipped; it is retried at the
            // next emission.
            (void)directControl->rxCaptureBufferTelemetry.CompleteInterval(now);
            // Stamped on every emission, so an anomaly burst defers the next
            // heartbeat instead of interleaving with it. Anomalies are never
            // themselves suppressed.
            directControl->txHeartbeatLastHostTicks.store(
                now, std::memory_order_relaxed);
            // Kept under the driver ring's 232-byte message. Fill health and
            // the CoreAudio budget first (TX_OWNERSHIP.md, experiment E1):
            // sOutMinPk = smallest S_out headroom this interval, in packets
            //   ahead of the finality frontier (-1: nothing was filled);
            // sInMinFr = smallest S_in headroom, capture frames already
            //   written past the HAL read end (-1: no reads);
            // sInStarve = capture starvations this interval after start-up;
            // sInStart = reads that starved during start-up (before the
            //   stream's first complete read and 2 s after it), for the whole
            //   stream.
            const uint64_t missedNow =
                ivars->runtime.txStreamEngine.PayloadWriterCounters()
                    .framesMissedFinality.load(std::memory_order_relaxed) +
                ivars->runtime.motuPayloadWriter.Counters()
                    .framesMissedFinality.load(std::memory_order_relaxed);
            // Each stream fills through one of the two writers; the other
            // reports nothing (INT64_MAX).
            const int64_t sOutMin =
                std::min(ivars->runtime.txStreamEngine.TakeMinFinalityMarginPackets(),
                         ivars->runtime.motuPayloadWriter.TakeMinFinalityMarginPackets());
            // The receive side also observes the capture ring, so without a
            // CoreAudio read in the interval the minimum is just the ring
            // capacity. Headroom only exists relative to a read.
            const bool inputRead =
                directControl->rxCaptureBufferTelemetry.completedReaderBeginReadCalls.load(
                    std::memory_order_relaxed) != 0;
            const uint64_t sInMin = inputRead
                ? directControl->rxCaptureBufferTelemetry.completedMinimumAvailableFrames.load(
                      std::memory_order_relaxed)
                : UINT64_MAX;
            ASFW_LOG_DEBUG(
                DirectAudio,
                "[TxPrep] forcedNoData=%llu missedFinality=%llu sOutMinPk=%lld "
                "sInMinFr=%lld sInStarve=%llu sInStart=%llu margin=%u min=%u latUs=%llu/%llu/%llu "
                "late1500=%llu wakes=%llu fillUs=%llu%{public}s",
                directControl->txReplayForcedNoData.load(std::memory_order_relaxed),
                missedNow >= ivars->runtime.txMissedFinalityAtStart
                    ? missedNow - ivars->runtime.txMissedFinalityAtStart
                    : 0,
                sOutMin == INT64_MAX ? -1LL : static_cast<long long>(sOutMin),
                sInMin == UINT64_MAX ? -1LL : static_cast<long long>(sInMin),
                directControl->rxCaptureBufferTelemetry.completedStarvationEvents.load(
                    std::memory_order_relaxed),
                directControl->captureRingStartupStarvations.load(std::memory_order_relaxed),
                boundedMargin,
                minCommittedMargin,
                latencyNanos / 1000,
                intervalLatencyMaxNanos / 1000,
                maxLatencyNanos / 1000,
                directControl->txPreparationAtLeast1500Us.load(
                    std::memory_order_relaxed),
                wakeSamples,
                ASFW::Timing::hostTicksToNanos(
                    ivars->runtime.txFillMaxDurationTicks.exchange(
                        0, std::memory_order_relaxed)) / 1000,
                boundedMargin <= kCommittedMarginDangerPackets ? " DANGER" : "");
            // Content, for the silent-start investigation (the device locked
            // to our stream and playing nothing): did CoreAudio's samples
            // reach packet memory? Counters are cumulative for the stream;
            // peakQ24 is this interval's largest written |sample| (24-bit),
            // word its encoded slot. Primary stream only.
            // A MOTU stream fills through its own writer; the AMDTP writer's
            // counters stay zero for it (b107 report read them as "no PCM").
            const bool motuWriter = ivars->runtime.motuSphActive.load(std::memory_order_relaxed);
            const auto& fill = ivars->runtime.txStreamEngine.PayloadWriterCounters();
            const auto& motuFill = ivars->runtime.motuPayloadWriter.Counters();
            uint32_t peakWord = 0;
            const uint32_t peakQ24 = motuWriter
                ? ivars->runtime.motuPayloadWriter.TakePeakWrittenQ24()
                : ivars->runtime.txStreamEngine.TakePeakWrittenQ24(&peakWord);
            ASFW_LOG_DEBUG(DirectAudio,
                           "[TxContent] writer=%{public}s peakQ24=%u word=0x%08x visited=%llu written=%llu noPkt=%llu outside=%llu "
                           "missed=%llu muted=%llu writeEnd=%llu filledEnd=%llu",
                           motuWriter ? "motu" : "amdtp", peakQ24, peakWord,
                           motuWriter ? motuFill.framesVisited.load(std::memory_order_relaxed) : 0ULL,
                           motuWriter ? motuFill.framesWritten.load(std::memory_order_relaxed)
                                : fill.framesWritten.load(std::memory_order_relaxed),
                           motuWriter ? motuFill.framesWithoutPacket.load(std::memory_order_relaxed)
                                : fill.framesWithoutPacket.load(std::memory_order_relaxed),
                           motuWriter ? motuFill.framesOutsidePacket.load(std::memory_order_relaxed)
                                : fill.framesOutsidePacket.load(std::memory_order_relaxed),
                           motuWriter ? motuFill.framesMissedFinality.load(std::memory_order_relaxed)
                                : fill.framesMissedFinality.load(std::memory_order_relaxed),
                           motuWriter ? motuFill.framesMuted.load(std::memory_order_relaxed) : 0ULL,
                           directControl->playbackRingWriteFrame.load(std::memory_order_relaxed),
                           directControl->playbackRingReadFrame.load(std::memory_order_relaxed));
            // The gate the fill waits on (FillTransmitPayloads copies frames in
            // [filled, min(writtenEnd, exposedEnd)) only): CoreAudio's write
            // end, the ring's oldest held frame, the timeline's exposed frame
            // end (one past the newest DATA packet's frames), the engine's
            // frame cursor and its alignment epoch, plus what the engine made.
            {
                const auto& engine = ivars->runtime.txStreamEngine;
                ASFW_LOG_DEBUG(DirectAudio,
                               "[TxFill] writtenEnd=%llu oldestValid=%llu exposedEnd=%llu filled=%llu cursor=%llu "
                               "epoch=%llu aligned=%u dataPrepared=%llu noDataPrepared=%llu timingReverts=%llu "
                               "replayActive=%u replayEstablished=%u",
                               directControl->playbackRingWriteFrame.load(std::memory_order_relaxed),
                               directControl->playbackRingOldestValidFrame.load(std::memory_order_relaxed),
                               engine.Timeline().ExposedFrameEnd(), ivars->runtime.txFilledFrameEnd,
                               engine.NextAudioFrame(), engine.CursorEpoch(), engine.IsFrameCursorAligned() ? 1U : 0U,
                               engine.Counters().dataPacketsPrepared.load(std::memory_order_relaxed),
                               engine.Counters().noDataPacketsPrepared.load(std::memory_order_relaxed),
                               engine.Counters().timingUnavailableReverts.load(std::memory_order_relaxed),
                               ivars->runtime.txReplayReader.IsActive() ? 1U : 0U,
                               directControl->rxSequenceReplay.IsEstablished() ? 1U : 0U);
            }
            // Whether CoreAudio runs IO on the started device at all, beside
            // the zero timestamps handed to it: callbacks stuck at 0 while zts
            // climbs and ztsAgeMs stays under a period is the HAL not cycling
            // (b152: f717 at 96 kHz after refused 44.1/88.2 kHz starts).
            {
                const uint64_t ztsHost =
                    ivars->runtime.lastHalZeroTimestampHostTicks.load(std::memory_order_relaxed);
                // pulls: BeginRead pulls since the hook was built, the packets
                // they drained, those that gave up waiting for a poll in
                // progress (busy) and those cut short at their deadline (cut)
                // (RxPullHook.hpp).
                ASFW_LOG_DEBUG(DirectAudio,
                               "[HalIo] callbacks=%llu outsideRun=%llu zts=%llu ztsFrame=%llu ztsAgeMs=%llu "
                               "pulls=%llu/%llu busy=%llu cut=%llu",
                               ivars->runtime.ioDebugCallbacks.load(std::memory_order_relaxed),
                               ivars->runtime.ioCallbacksOutsideRun.load(std::memory_order_relaxed),
                               directControl->hostClockAnchor.mirrorPublications.load(std::memory_order_relaxed),
                               ivars->runtime.lastHalZeroTimestampSampleFrame.load(std::memory_order_relaxed),
                               ztsHost != 0 && now > ztsHost
                             ? ASFW::Timing::hostTicksToNanos(now - ztsHost) / 1000000
                             : 0ULL,
                               directControl->rxPull.pulls.load(std::memory_order_relaxed),
                               directControl->rxPull.pulledPackets.load(std::memory_order_relaxed),
                               directControl->rxPull.busy.load(std::memory_order_relaxed),
                               directControl->rxPull.cut.load(std::memory_order_relaxed));
                // The driver's share of the HAL's IO cycle, this interval:
                // the longest BeginRead and WriteEnd here (us); for input, how
                // long before BeginRead its last frame arrived by our zero
                // timestamps (least, us; the input safety offset when the HAL
                // reads where we expect) and the HAL's host time for the read
                // against ours (least/most, us). See ASFWAudioDriverIO.cpp.
                if (ASFW_LOG_ENABLED(DirectAudio, Debug)) {
                    auto& runtime = ivars->runtime;
                    const int64_t leadNanos =
                        runtime.ioInputLeadMinNanos.exchange(INT64_MAX, std::memory_order_relaxed);
                    const int64_t leadMaxNanos =
                        runtime.ioInputLeadMaxNanos.exchange(INT64_MIN, std::memory_order_relaxed);
                    const int64_t skewMin =
                        runtime.ioHalSkewMinNanos.exchange(INT64_MAX, std::memory_order_relaxed);
                    const int64_t skewMax =
                        runtime.ioHalSkewMaxNanos.exchange(INT64_MIN, std::memory_order_relaxed);
                    const int64_t jumpMin =
                        runtime.ztsJumpMinNanos.exchange(INT64_MAX, std::memory_order_relaxed);
                    const int64_t jumpMax =
                        runtime.ztsJumpMaxNanos.exchange(INT64_MIN, std::memory_order_relaxed);
                    const int64_t outMargin =
                        runtime.ioOutMarginMinNanos.exchange(INT64_MAX, std::memory_order_relaxed);
                    // inLeadUs least/most and inLeadNeg (reads whose last frame
                    // had not arrived by our zero timestamps); ahead = reads
                    // ahead of the arrivals (kept out of the lead figures);
                    // outMarginUs = the least HAL margin at a WriteEnd's entry;
                    // ztsJumpUs = the steps between zero timestamps against
                    // the nominal rate.
                    ASFW_LOG_DEBUG(DirectAudio,
                                   "[HalIoTime] beginReadMaxUs=%llu writeEndMaxUs=%llu inLeadUs=%lld/%lld "
                                   "inLeadNeg=%llu ahead=%llu outMarginUs=%lld halSkewUs=%lld/%lld "
                                   "ztsJumpUs=%lld/%lld",
                                   ASFW::Timing::hostTicksToNanos(runtime.ioBeginReadMaxTicks.exchange(
                                       0, std::memory_order_relaxed)) / 1000,
                                   ASFW::Timing::hostTicksToNanos(runtime.ioWriteEndMaxTicks.exchange(
                                       0, std::memory_order_relaxed)) / 1000,
                                   leadNanos == INT64_MAX ? 0LL : static_cast<long long>(leadNanos / 1000),
                                   leadMaxNanos == INT64_MIN ? 0LL : static_cast<long long>(leadMaxNanos / 1000),
                                   runtime.ioInputLeadNegative.exchange(0, std::memory_order_relaxed),
                                   runtime.ioAheadReads.exchange(0, std::memory_order_relaxed),
                                   outMargin == INT64_MAX ? 0LL : static_cast<long long>(outMargin / 1000),
                                   skewMin == INT64_MAX ? 0LL : static_cast<long long>(skewMin / 1000),
                                   skewMax == INT64_MIN ? 0LL : static_cast<long long>(skewMax / 1000),
                                   jumpMin == INT64_MAX ? 0LL : static_cast<long long>(jumpMin / 1000),
                                   jumpMax == INT64_MIN ? 0LL : static_cast<long long>(jumpMax / 1000));
                }
            }
            // Cadence per interval: at 48 kHz blocking the device-replayed
            // pattern is 6 DATA : 2 NO-DATA per 8 cycles (6000 data packets/s
            // of 8 frames); 8:0 would push 64 000 frames/s (b92 report §2).
            {
                const uint64_t dataNow = directControl->counters.txDataPackets.load(std::memory_order_relaxed);
                const uint64_t noDataNow = directControl->counters.txNoDataPackets.load(std::memory_order_relaxed);
                const uint64_t emptyNow = directControl->counters.txEmptyPackets.load(std::memory_order_relaxed);
                const uint64_t data = dataNow - ivars->runtime.txCadenceLastData;
                const uint64_t noData = noDataNow - ivars->runtime.txCadenceLastNoData;
                const uint64_t empty = emptyNow - ivars->runtime.txCadenceLastEmpty;
                ivars->runtime.txCadenceLastData = dataNow;
                ivars->runtime.txCadenceLastNoData = noDataNow;
                ivars->runtime.txCadenceLastEmpty = emptyNow;
                const uint64_t total = data + noData + empty;
                ASFW_LOG_DEBUG(DirectAudio, "[TxCadence] data=%llu noData=%llu empty=%llu dataPerMille=%llu (expect 750 at 48 kHz)",
                               data, noData, empty, total ? (data * 1000) / total : 0);
            }
            if (ivars->runtime.motuSphActive.load(std::memory_order_relaxed)) {
                ASFW::Audio::DriverKit::LogMotuSphSummary(*ivars);
            }
        }

        directControl->counters.txPreparationWakeRequests.store(
            txControl->refillRequestCount.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
        directControl->counters.txPreparationWakeDispatches.fetch_add(
            1, std::memory_order_relaxed);
        directControl->counters.txPreparationWakeCoalesced.store(
            txControl->refillCoalescedCount.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
        directControl->counters.txPreparationDrainPasses.fetch_add(
            1, std::memory_order_relaxed);
        // Every pass fills what CoreAudio has written, so every pass handles
        // the request it saw.
        directControl->txPreparationRequests.MarkHandled(audioRequested, now);
        directControl->txPreparationRequests.FinishWake();
        // A CoreAudio callback can publish while this action is preparing
        // slots. It saw wakeScheduled=true and deliberately did not enqueue a
        // second action; hand it one now after draining the latest target.
        scheduleAudioFollowUp =
            directControl->txPreparationRequests.NeedsHandling() &&
            directControl->txPreparationRequests.TryScheduleWake();
    }

    txControl->MarkRefillHandled(requested);

    // [TxPlace]: where a transmitted frame sits against the HAL clock
    // (milestone 6, documentation/TX_OWNERSHIP.md). The first measurement of
    // each stream only (T8); the start-to-start difference is what matters.
    if (!ivars->runtime.txPlacementLogged) {
        ASFW::Audio::DriverKit::TxPlacementSample placement{};
        if (ASFW::Audio::DriverKit::MeasureTxPlacement(*ivars, placement)) {
            ivars->runtime.txPlacementLogged = true;
            ASFW_LOG_DEBUG(DirectAudio,
                           "[TxPlace] pkt=%llu frame=%llu halSample=%lld offset=%lld rate=%u",
                           placement.packetIndex, placement.firstAudioFrame,
                           placement.halSampleTime, placement.offsetFrames,
                           ivars->runtime.txStreamEngine.StreamConfig().sampleRate);
        }
    }

    if (scheduleAudioFollowUp && ivars->device.audioNub && ivars->txPreparationAction) {
        // Same direct, one-way OSAction path as the hardware refill callback.
        ivars->device.audioNub->TxPreparationReady(ivars->txPreparationAction.get(),
            directControl->txPreparationRequests.RequestedGeneration());
    }
}
