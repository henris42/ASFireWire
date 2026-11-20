//
// ASFWAudioDriverIO.cpp
// ASFWDriver
//
// Real-time IO callback installation for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

#include <algorithm>
#include <cstring>

namespace ASFW::Audio::DriverKit {
namespace {

// The BeginRead pull's time budget, for waiting out a poll in progress (the
// interrupt draining the same ring) and for its own drain together: an eighth
// of the IO buffer, within these bounds. At 64 frames and 96 kHz the HAL
// allows about 760 us for the whole cycle, client and driver (b184 report).
constexpr uint64_t kRxPullMinBudgetNanos = 25'000;
constexpr uint64_t kRxPullMaxBudgetNanos = 100'000;

[[nodiscard]] uint64_t RxPullBudgetNanos(uint32_t frames, uint32_t sampleRateHz) noexcept {
    if (sampleRateHz == 0) {
        return kRxPullMinBudgetNanos;
    }
    const uint64_t bufferNanos = (static_cast<uint64_t>(frames) * 1'000'000'000ULL) / sampleRateHz;
    return std::clamp(bufferNanos / 8, kRxPullMinBudgetNanos, kRxPullMaxBudgetNanos);
}

// The IO operation that returned last, on any device of this process
// (IoOpTiming.hpp, "follow").
ASFW::Audio::Runtime::IoOpLastReturn gLastIoReturn;

template <typename T>
void RecordMax(std::atomic<T>& slot, T value) noexcept {
    if (value > slot.load(std::memory_order_relaxed)) {
        slot.store(value, std::memory_order_relaxed);
    }
}

template <typename T>
void RecordMin(std::atomic<T>& slot, T value) noexcept {
    if (value < slot.load(std::memory_order_relaxed)) {
        slot.store(value, std::memory_order_relaxed);
    }
}

// Where a read sits on our timeline: the host time of a frame by the last
// zero timestamp we gave CoreAudio, at the nominal rate. lead = how long
// before `nowTicks` the read's last frame arrived (negative: not yet), skew =
// CoreAudio's host time for the read's first frame minus ours.
struct InputTiming {
    bool valid{false};
    int64_t leadNanos{0};
    int64_t skewNanos{0};
    bool haveSkew{false};
};

[[nodiscard]] InputTiming MeasureInputTiming(const ASFWAudioDriver_IVars& ivars, uint64_t sampleTime,
                                             uint32_t frames, uint64_t halHostTime,
                                             uint64_t nowTicks) noexcept {
    const auto& runtime = ivars.runtime;
    const uint64_t ztsHost = runtime.lastHalZeroTimestampHostTicks.load(std::memory_order_relaxed);
    const uint64_t ztsFrame = runtime.lastHalZeroTimestampSampleFrame.load(std::memory_order_relaxed);
    const uint32_t rate = runtime.directAudioGraph.sampleRateHz;
    if (ztsHost == 0 || rate == 0) {
        return {};
    }
    const auto ourNanos = [&](uint64_t frame) noexcept {
        const int64_t framesFromZts = static_cast<int64_t>(frame) - static_cast<int64_t>(ztsFrame);
        return static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(ztsHost)) +
               framesFromZts * 1'000'000'000LL / static_cast<int64_t>(rate);
    };
    InputTiming timing{.valid = true};
    timing.leadNanos = static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(nowTicks)) -
                       ourNanos(sampleTime + frames);
    if (halHostTime != 0) {
        timing.skewNanos = static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(halHostTime)) -
                           ourNanos(sampleTime);
        timing.haveSkew = true;
    }
    return timing;
}

// Debug level only ([HalIoTime]).
void RecordInputTiming(ASFWAudioDriver_IVars& ivars, uint64_t sampleTime, uint32_t frames,
                       uint64_t halHostTime, uint64_t nowTicks) noexcept {
    auto& runtime = ivars.runtime;
    const InputTiming timing = MeasureInputTiming(ivars, sampleTime, frames, halHostTime, nowTicks);
    if (!timing.valid) {
        return;
    }
    RecordMin(runtime.ioInputLeadMinNanos, timing.leadNanos);
    RecordMax(runtime.ioInputLeadMaxNanos, timing.leadNanos);
    if (timing.leadNanos < 0) {
        runtime.ioInputLeadNegative.fetch_add(1, std::memory_order_relaxed);
    }
    if (timing.haveSkew) {
        RecordMin(runtime.ioHalSkewMinNanos, timing.skewNanos);
        RecordMax(runtime.ioHalSkewMaxNanos, timing.skewNanos);
    }
}

[[nodiscard]] int32_t ClampToInt32(int64_t value) noexcept {
    return static_cast<int32_t>(std::clamp<int64_t>(value, INT32_MIN, INT32_MAX));
}

// A BeginRead's place in the stream and around it, for a short read's event.
struct BeginReadContext {
    uint64_t sampleTime{0};
    uint32_t frames{0};
    uint64_t halHostTime{0};
    uint64_t entryTicks{0};
    uint64_t previousFrame{0};
    uint64_t previousTicks{0};
    uint64_t writeBefore{0};
    uint16_t thread{0};
};

// A read that came up short after start-up, or lay ahead of the arrivals,
// goes into the [Glitch] event log with everything that tells an early read
// from a late packet.
void RecordReadEvent(ASFWAudioDriver_IVars& ivars, const BeginReadContext& read,
                     const ASFW::Audio::Runtime::RxPullResult& pull, uint64_t writeAfter,
                     ASFW::Audio::Runtime::GlitchEventKind kind) noexcept {
    using ASFW::Audio::Runtime::GlitchEvent;
    const uint64_t readEnd = read.sampleTime + read.frames;
    GlitchEvent event{};
    event.sampleFrame = read.sampleTime;
    event.hostTicks = read.entryTicks;
    event.kind = kind;
    event.pull = pull.outcome;
    event.frames = static_cast<uint16_t>(std::min<uint32_t>(read.frames, UINT16_MAX));
    event.missing = static_cast<uint16_t>(
        std::min<uint64_t>(writeAfter < readEnd ? readEnd - writeAfter : 0, UINT16_MAX));
    event.drained = static_cast<uint16_t>(std::min<uint32_t>(pull.drained, UINT16_MAX));
    event.thread = read.thread;
    event.gapBefore = ClampToInt32(static_cast<int64_t>(readEnd) - static_cast<int64_t>(read.writeBefore));
    const InputTiming timing =
        MeasureInputTiming(ivars, read.sampleTime, read.frames, read.halHostTime, read.entryTicks);
    if (timing.valid) {
        event.leadUs = ClampToInt32(timing.leadNanos / 1000);
        event.skewUs = timing.haveSkew ? ClampToInt32(timing.skewNanos / 1000) : 0;
    }
    if (read.previousTicks != 0) {
        event.prevFrames = ClampToInt32(static_cast<int64_t>(read.sampleTime) -
                                        static_cast<int64_t>(read.previousFrame));
        event.prevUs = ClampToInt32(static_cast<int64_t>(
            ASFW::Timing::hostTicksToNanos(read.entryTicks - std::min(read.entryTicks, read.previousTicks)) /
            1000));
    }
    const uint64_t published =
        ivars.runtime.lastHalZeroTimestampPublishTicks.load(std::memory_order_relaxed);
    if (published != 0 && read.entryTicks > published) {
        event.ztsAgeUs = static_cast<uint32_t>(std::min<uint64_t>(
            ASFW::Timing::hostTicksToNanos(read.entryTicks - published) / 1000, UINT32_MAX));
    }
    ivars.runtime.glitchEvents.Record(event);
}

// What a BeginRead pull drains for: the capture ring's write frame reaching
// the end of the read.
struct RxPullTarget {
    const std::atomic<uint64_t>* writeFrame{nullptr};
    uint64_t readEnd{0};

    static bool Covered(const void* context) noexcept {
        const auto* target = static_cast<const RxPullTarget*>(context);
        return target->writeFrame->load(std::memory_order_acquire) >= target->readEnd;
    }
};
// After the first complete read, CoreAudio's clock for the device still
// converges on the zero timestamps (one per 24576 frames, 256 ms at 96 kHz):
// b184 had one short capture read in the first seconds of nearly every start
// and none in 120 s of steady state. Short reads this soon are start-up.
constexpr uint64_t kCaptureSettleNanos = 2'000'000'000ULL;

void ZeroInputFrameIfMissing(ASFW::Audio::Runtime::AudioGraphBinding& graph,
                             uint64_t absoluteFrame) noexcept {
    auto* frame = graph.memory.InputFrame(absoluteFrame);
    if (!frame || graph.memory.inputChannels == 0) {
        return;
    }
    std::memset(frame,
                0,
                static_cast<size_t>(graph.memory.inputChannels) * sizeof(int32_t));
}

// A read ahead of the arrivals (GlitchEventKind::ReadAhead) gets silence for
// every frame: none has arrived, and the ring there still holds the frames of
// a lap ago. The receive side overwrites the slots when the frames do arrive.
void ZeroInputFrames(ASFW::Audio::Runtime::AudioGraphBinding& graph, uint64_t sampleTime,
                     uint32_t frameCount) noexcept {
    if (!graph.HasInput()) {
        return;
    }
    for (uint32_t i = 0; i < frameCount; ++i) {
        ZeroInputFrameIfMissing(graph, sampleTime + i);
    }
}

// What a BeginRead found in the capture ring: the write position it read
// against, and whether frames were missing after start-up (a glitch).
struct CaptureRead {
    uint64_t write{0};
    bool shortAfterStartup{false};
};

CaptureRead PrepareCaptureRingForBeginRead(ASFW::Audio::Runtime::AudioGraphBinding& graph,
                                           ASFW::Audio::Runtime::AudioTransportControlBlock& control,
                                           uint64_t sampleTime,
                                           uint32_t frameCount) noexcept {
    if (frameCount == 0 || !graph.HasInput()) {
        return {};
    }

    const uint64_t write =
        control.captureRingWriteFrame.load(std::memory_order_acquire);
    const uint32_t capacity = graph.memory.inputFrameCapacity;
    const uint64_t oldest = (capacity != 0 && write > capacity) ? (write - capacity) : 0;
    bool starved = false;
    uint32_t starvedFrames = 0;
    for (uint32_t i = 0; i < frameCount; ++i) {
        const uint64_t frame = sampleTime + i;
        if (frame < oldest || frame >= write) {
            ZeroInputFrameIfMissing(graph, frame);
            starved = true;
            ++starvedFrames;
        }
    }

    const uint64_t readEnd = sampleTime + frameCount;
    const uint64_t previousRead =
        control.captureRingReadFrame.load(std::memory_order_acquire);
    if (readEnd > previousRead) {
        control.captureRingReadFrame.store(readEnd, std::memory_order_release);
    }
    if (starved) {
        control.captureRingStarvations.fetch_add(1, std::memory_order_relaxed);
    }
    // Until the first complete read the capture ring is still filling behind
    // the HAL clock, and for kCaptureSettleNanos after it the HAL clock still
    // settles: count short reads then as start-up, not as S_in headroom.
    const uint64_t settledFrom =
        control.captureRingSettledHostTicks.load(std::memory_order_relaxed);
    if (settledFrom == 0 || mach_absolute_time() < settledFrom) {
        if (starved) {
            control.captureRingStartupStarvations.fetch_add(1, std::memory_order_relaxed);
            return {.write = write};
        }
        if (settledFrom == 0) {
            control.captureRingSettledHostTicks.store(
                mach_absolute_time() + ASFW::Timing::nanosToHostTicks(kCaptureSettleNanos),
                std::memory_order_relaxed);
        }
    }
    if (starved) {
        control.rxCaptureBufferTelemetry.RecordStarvation(starvedFrames);
    }
    control.rxCaptureBufferTelemetry.Observe(
        write, readEnd, capacity);
    return {.write = write, .shortAfterStartup = starved};
}

} // namespace

kern_return_t InstallIOOperationHandler(IOUserAudioDevice& audioDevice,
                                        ASFWAudioDriver_IVars& ivars) noexcept {
    auto* driverIvars = &ivars;
    const kern_return_t error = audioDevice.SetIOOperationHandler(
        ^kern_return_t(IOUserAudioObjectID           objectID,
                       IOUserAudioIOOperation        operation,
                       uint32_t                      ioBufferFrameSize,
                       uint64_t                      sampleTime,
                       uint64_t                      hostTime)
    {
        if (!driverIvars) {
            return kIOReturnNotReady;
        }

        auto* graphControl = driverIvars->runtime.directAudioGraph.control;
        auto& callbackState = graphControl
            ? *graphControl
            : driverIvars->runtime.directAudioControl;

        auto returnError = [&](kern_return_t kr) noexcept {
            callbackState.ioLastError.store(
                static_cast<uint32_t>(kr), std::memory_order_relaxed);
            callbackState.ioLastErrorOperation.store(
                static_cast<uint32_t>(operation), std::memory_order_relaxed);
            callbackState.ioLastErrorFrameCount.store(
                ioBufferFrameSize, std::memory_order_relaxed);
            callbackState.ioLastErrorObjectId.store(
                objectID, std::memory_order_relaxed);
            callbackState.ioLastErrorSampleTime.store(
                sampleTime, std::memory_order_relaxed);
            callbackState.ioLastErrorHostTime.store(
                hostTime, std::memory_order_relaxed);
            callbackState.ioCallbackErrorGeneration.fetch_add(
                1, std::memory_order_release);
            return kr;
        };

        (void)driverIvars->runtime.ioDebugCallbacks.fetch_add(1, std::memory_order_relaxed);
        callbackState.ioLastOperation.store(
            static_cast<uint32_t>(operation), std::memory_order_relaxed);
        callbackState.ioLastFrameCount.store(
            ioBufferFrameSize, std::memory_order_relaxed);
        callbackState.ioLastObjectId.store(
            objectID, std::memory_order_relaxed);
        callbackState.ioLastSampleTime.store(
            sampleTime, std::memory_order_relaxed);
        callbackState.ioLastHostTime.store(
            hostTime, std::memory_order_relaxed);
        callbackState.ioCallbackGeneration.fetch_add(
            1, std::memory_order_release);

        const bool running = driverIvars->runtime.isRunning.load(std::memory_order_acquire);
        const bool skeletonBound =
            driverIvars->runtime.directAudioSkeletonBound.load(std::memory_order_acquire);

        if (!running) {
            driverIvars->runtime.ioCallbacksOutsideRun.fetch_add(
                1, std::memory_order_relaxed);
            return kIOReturnSuccess;
        }

        if (skeletonBound) {
            auto* control = graphControl;
            if (!control) {
                return returnError(kIOReturnNotReady);
            }

            const bool timeIo = ASFW_LOG_ENABLED(DirectAudio, Debug);
            // The handler's own time is measured at every log level
            // (IoOpTiming.hpp): this clock read and one at the return.
            const uint64_t ioStartTicks = mach_absolute_time();
            // The IO thread's tag, taken here where every operation on a
            // thread has the same frame (GlitchEventLog.hpp).
            const uint16_t threadTag = ASFW::Audio::Runtime::CurrentThreadTag();
            auto& ioTiming = driverIvars->runtime.ioTiming;
            const bool isWriteEnd = operation == IOUserAudioIOOperationWriteEnd;
            // How soon this operation began after the same operation returned
            // on another device (an aggregate runs them back to back).
            {
                const uintptr_t previousTag = gLastIoReturn.tag.load(std::memory_order_relaxed);
                const uint64_t previousTicks = gLastIoReturn.ticks.load(std::memory_order_relaxed);
                if (ASFW::Audio::Runtime::IoOpFollows(previousTag, driverIvars, isWriteEnd) &&
                    ioStartTicks > previousTicks) {
                    ioTiming.RecordFollow(ioStartTicks - previousTicks);
                }
            }
            // When CoreAudio got here, against the host time it gives for the
            // operation's first frame; and for a WriteEnd, how long since this
            // device's BeginRead returned (the client's render).
            if (isWriteEnd) {
                ioTiming.RecordArrival(true, ioStartTicks, hostTime);
                ioTiming.RecordWriteEntry(ioStartTicks, ioBufferFrameSize);
            } else if (operation == IOUserAudioIOOperationBeginRead) {
                ioTiming.RecordArrival(false, ioStartTicks, hostTime);
            }
            // The operation's return: its time, with its pull's or its fill's
            // (`partTicks`), into the figures; a slow one is kept whole, and at
            // debug level every one goes into the IO trace (`capture`: with
            // the trace before it captured for the [IoOps] lines). Returns the
            // operation's time.
            const auto finishOp = [&](ASFW::Audio::Runtime::IoStepTiming& step, uint64_t partTicks,
                                      bool capture) noexcept {
                const uint64_t endTicks = mach_absolute_time();
                const uint64_t took = endTicks - ioStartTicks;
                step.Record(took);
                const bool slow = took >= ioTiming.slowTicks;
                if (slow || timeIo) {
                    const auto nanos = [](uint64_t ticks) noexcept {
                        return static_cast<uint32_t>(
                            std::min<uint64_t>(ASFW::Timing::hostTicksToNanos(ticks), UINT32_MAX));
                    };
                    const ASFW::Audio::Runtime::IoOp op{
                        .entryTicks = ioStartTicks,
                        .sampleTime = sampleTime,
                        .hostTime = hostTime,
                        .frames = ioBufferFrameSize,
                        .durationNanos = nanos(took),
                        .partNanos = nanos(partTicks),
                        .operation = static_cast<uint16_t>(operation),
                        .thread = threadTag,
                    };
                    if (slow) {
                        ioTiming.RecordSlow(op);
                    }
                    if (timeIo) {
                        auto& runtime = driverIvars->runtime;
                        if (capture) {
                            (void)runtime.ioOpTraceCapture.TryCapture(runtime.ioOpTrace, op);
                        }
                        runtime.ioOpTrace.Record(op);
                    }
                }
                gLastIoReturn.tag.store(
                    ASFW::Audio::Runtime::IoOpFollowTag(driverIvars, isWriteEnd),
                    std::memory_order_relaxed);
                gLastIoReturn.ticks.store(endTicks, std::memory_order_relaxed);
                if (!isWriteEnd) {
                    ioTiming.RecordReadReturn(endTicks);
                }
                return took;
            };
            if (operation == IOUserAudioIOOperationBeginRead) {
                // ADK permits operation spans that differ from the nominal IO
                // size. The stream ring capacity is the actual hard bound.
                if (ioBufferFrameSize >
                    driverIvars->runtime.directAudioGraph.memory.inputFrameCapacity) {
                    return returnError(kIOReturnBadArgument);
                }
                auto& runtime = driverIvars->runtime;
                const uint64_t entryTicks = ioStartTicks;
                const BeginReadContext readContext{
                    .sampleTime = sampleTime,
                    .frames = ioBufferFrameSize,
                    .halHostTime = hostTime,
                    .entryTicks = entryTicks,
                    .previousFrame =
                        runtime.lastBeginReadFrame.exchange(sampleTime, std::memory_order_relaxed),
                    .previousTicks =
                        runtime.lastBeginReadTicks.exchange(entryTicks, std::memory_order_relaxed),
                    .writeBefore = control->captureRingWriteFrame.load(std::memory_order_acquire),
                    .thread = threadTag,
                };
                // A read whose first frame lies at or past the write position,
                // after start-up: nothing of it has arrived, so a pull cannot
                // serve it and it is not S_in headroom. It reads silence and
                // leaves the read position and the telemetry alone. b190 saw
                // these only with two clients on a unit, at the output write's
                // position; the event and the IO trace (debug) show the HAL's
                // operations around it.
                const uint64_t settledFrom =
                    control->captureRingSettledHostTicks.load(std::memory_order_relaxed);
                if (settledFrom != 0 && entryTicks >= settledFrom &&
                    sampleTime >= readContext.writeBefore) {
                    ZeroInputFrames(runtime.directAudioGraph, sampleTime, ioBufferFrameSize);
                    control->captureRingAheadReads.fetch_add(1, std::memory_order_relaxed);
                    RecordReadEvent(*driverIvars, readContext, {}, readContext.writeBefore,
                                    ASFW::Audio::Runtime::GlitchEventKind::ReadAhead);
                    control->counters.CountBeginRead();
                    const uint64_t took = finishOp(ioTiming.beginRead, 0, /*capture=*/true);
                    if (timeIo) {
                        runtime.ioAheadReads.fetch_add(1, std::memory_order_relaxed);
                        RecordMax(runtime.ioBeginReadMaxTicks, took);
                    }
                    return kIOReturnSuccess;
                }
                control->client.PublishBeginRead(sampleTime, hostTime, ioBufferFrameSize);
                control->rxCaptureBufferTelemetry.RecordReaderBeginRead();
                // The frames this read needs are not all in the capture ring:
                // drain the receive ring here rather than wait for the
                // interrupt, which a real-time load can hold off for
                // milliseconds (RxPullHook.hpp); only up to this read's end,
                // within a budget, leaving any backlog to the interrupt.
                const RxPullTarget pullTarget{
                    .writeFrame = &control->captureRingWriteFrame,
                    .readEnd = sampleTime + ioBufferFrameSize,
                };
                ASFW::Audio::Runtime::RxPullResult pull{
                    .outcome = ASFW::Audio::Runtime::RxPullOutcome::Covered};
                uint64_t pullTicks = 0;
                if (readContext.writeBefore < pullTarget.readEnd) {
                    const uint64_t budgetNanos = RxPullBudgetNanos(
                        ioBufferFrameSize, runtime.directAudioGraph.sampleRateHz);
                    const uint64_t pullStarted = mach_absolute_time();
                    pull = control->rxPull.Pull({
                        .enough = &RxPullTarget::Covered,
                        .context = &pullTarget,
                        .deadlineTicks = pullStarted + ASFW::Timing::nanosToHostTicks(budgetNanos),
                    });
                    pullTicks = mach_absolute_time() - pullStarted;
                    ioTiming.pull.Record(pullTicks);
                }
                const CaptureRead read = PrepareCaptureRingForBeginRead(
                    runtime.directAudioGraph, *control, sampleTime, ioBufferFrameSize);
                if (read.shortAfterStartup) {
                    RecordReadEvent(*driverIvars, readContext, pull, read.write,
                                    ASFW::Audio::Runtime::GlitchEventKind::ShortCaptureRead);
                }
                control->counters.CountBeginRead();
                const uint64_t took = finishOp(ioTiming.beginRead, pullTicks, /*capture=*/false);
                if (timeIo) {
                    RecordInputTiming(*driverIvars, sampleTime, ioBufferFrameSize, hostTime,
                                      ioStartTicks);
                    RecordMax(runtime.ioBeginReadMaxTicks, took);
                }
            } else if (operation == IOUserAudioIOOperationWriteEnd) {
                // See BeginRead above: CoreAudio may choose a larger span than
                // kHalIoPeriodFrames while remaining within the stream ring.
                ioTiming.lastFillTicks.store(0, std::memory_order_relaxed);
                if (!HandleOutputWriteEnd(*driverIvars, *control, sampleTime, hostTime,
                                          ioBufferFrameSize, ioStartTicks, threadTag)) {
                    return returnError(kIOReturnBadArgument);
                }
                const uint64_t took =
                    finishOp(ioTiming.writeEnd, ioTiming.lastFillTicks.load(std::memory_order_relaxed),
                             /*capture=*/false);
                if (timeIo) {
                    auto& runtime = driverIvars->runtime;
                    RecordMax(runtime.ioWriteEndMaxTicks, took);
                    // The HAL's margin: its presentation time for the write's
                    // first frame against our entry ([HalIoTime] outMarginUs).
                    if (hostTime != 0) {
                        const int64_t marginNanos =
                            hostTime >= ioStartTicks
                                ? static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(hostTime - ioStartTicks))
                                : -static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(ioStartTicks - hostTime));
                        RecordMin(runtime.ioOutMarginMinNanos, marginNanos);
                    }
                }
            } else {
                return returnError(kIOReturnBadArgument);
            }
        } else {
            return returnError(kIOReturnNotReady);
        }

        return kIOReturnSuccess;
    });

    if (error == kIOReturnSuccess) {
        ASFW_LOG(DirectAudio,
                 "ADK IO handler installed deviceId=%u inputStream=%u outputStream=%u",
                 audioDevice.GetObjectID(),
                 ivars.inputStream ? ivars.inputStream->GetObjectID() : 0,
                 ivars.outputStream ? ivars.outputStream->GetObjectID() : 0);
    } else {
        ASFW_LOG_ERROR(Audio, "ASFWAudioDriver: SetIOOperationHandler failed: 0x%x", error);
    }
    return error;
}

} // namespace ASFW::Audio::DriverKit
