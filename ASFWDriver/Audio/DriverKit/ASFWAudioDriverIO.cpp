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

// Debug level only ([HalIoTime]): where this read sits on our timeline. The
// host time of a frame by the last zero timestamp we gave the HAL, at the
// nominal rate; lead = how long before `nowTicks` the read's last frame
// arrived, skew = the HAL's host time for its first frame minus ours.
void RecordInputTiming(ASFWAudioDriver_IVars& ivars, uint64_t sampleTime, uint32_t frames,
                       uint64_t halHostTime, uint64_t nowTicks) noexcept {
    auto& runtime = ivars.runtime;
    const uint64_t ztsHost = runtime.lastHalZeroTimestampHostTicks.load(std::memory_order_relaxed);
    const uint64_t ztsFrame = runtime.lastHalZeroTimestampSampleFrame.load(std::memory_order_relaxed);
    const uint32_t rate = runtime.directAudioGraph.sampleRateHz;
    if (ztsHost == 0 || rate == 0) {
        return;
    }
    const auto ourNanos = [&](uint64_t frame) noexcept {
        const int64_t framesFromZts = static_cast<int64_t>(frame) - static_cast<int64_t>(ztsFrame);
        return static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(ztsHost)) +
               framesFromZts * 1'000'000'000LL / static_cast<int64_t>(rate);
    };
    const int64_t nowNanos = static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(nowTicks));
    RecordMin(runtime.ioInputLeadMinNanos, nowNanos - ourNanos(sampleTime + frames));
    if (halHostTime != 0) {
        const int64_t skew = static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(halHostTime)) -
                             ourNanos(sampleTime);
        RecordMin(runtime.ioHalSkewMinNanos, skew);
        RecordMax(runtime.ioHalSkewMaxNanos, skew);
    }
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

bool PrepareCaptureRingForBeginRead(ASFW::Audio::Runtime::AudioGraphBinding& graph,
                                    ASFW::Audio::Runtime::AudioTransportControlBlock& control,
                                    uint64_t sampleTime,
                                    uint32_t frameCount) noexcept {
    if (frameCount == 0) {
        return true;
    }
    if (!graph.HasInput()) {
        return false;
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
            return true;
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
    return true;
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
            const uint64_t ioStartTicks = timeIo ? mach_absolute_time() : 0;
            if (operation == IOUserAudioIOOperationBeginRead) {
                // ADK permits operation spans that differ from the nominal IO
                // size. The stream ring capacity is the actual hard bound.
                if (ioBufferFrameSize >
                    driverIvars->runtime.directAudioGraph.memory.inputFrameCapacity) {
                    return returnError(kIOReturnBadArgument);
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
                if (!RxPullTarget::Covered(&pullTarget)) {
                    const uint64_t budgetNanos = RxPullBudgetNanos(
                        ioBufferFrameSize, driverIvars->runtime.directAudioGraph.sampleRateHz);
                    (void)control->rxPull.Pull({
                        .enough = &RxPullTarget::Covered,
                        .context = &pullTarget,
                        .deadlineTicks = mach_absolute_time() +
                                         ASFW::Timing::nanosToHostTicks(budgetNanos),
                    });
                }
                (void)PrepareCaptureRingForBeginRead(driverIvars->runtime.directAudioGraph,
                                                     *control,
                                                     sampleTime,
                                                     ioBufferFrameSize);
                control->counters.CountBeginRead();
                if (timeIo) {
                    RecordInputTiming(*driverIvars, sampleTime, ioBufferFrameSize, hostTime,
                                      ioStartTicks);
                    RecordMax(driverIvars->runtime.ioBeginReadMaxTicks,
                              mach_absolute_time() - ioStartTicks);
                }
            } else if (operation == IOUserAudioIOOperationWriteEnd) {
                // See BeginRead above: CoreAudio may choose a larger span than
                // kHalIoPeriodFrames while remaining within the stream ring.
                if (!HandleOutputWriteEnd(*driverIvars, *control, sampleTime, hostTime,
                                          ioBufferFrameSize)) {
                    return returnError(kIOReturnBadArgument);
                }
                if (timeIo) {
                    RecordMax(driverIvars->runtime.ioWriteEndMaxTicks,
                              mach_absolute_time() - ioStartTicks);
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
