//
// IoOpTiming.hpp
// ASFWDriver
//
// The IO handler's own time, measured at every log level: a clock read at
// each end of an operation. CoreAudio runs BeginRead and WriteEnd inside its
// IO cycle and waits for each to return, so the handler's time comes out of
// the cycle's budget (760 us at 64 frames and 96 kHz, b217); these figures say
// how much of it this driver took.
//
// Per device: BeginRead and WriteEnd (count, total, longest), within them the
// BeginRead pull (RxPullHook.hpp) and the transmit fill, and the operations
// that took the slow threshold or longer, kept one by one (slowOps). "Follow":
// how soon an operation began after the same operation returned on another
// device of this process. An aggregate device runs every unit's BeginRead back
// to back, then every unit's WriteEnd, so that gap is CoreAudio's and
// AudioDriverKit's way from one handler's return into the next handler, with
// nothing of ours in it.
//
// Written by the IO thread, read by the [IoTime] report on the transmit
// producer's thread: counts and totals only grow (the report keeps what it
// has printed), the longest of an interval is taken with an exchange.
//

#pragma once

#include "GlitchEventLog.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// An operation this long is reported at warn ([IoTime], [IoSlow]).
inline constexpr uint64_t kSlowIoOpNanos = 50'000;
// A gap this long is the IO cycle itself, not two operations back to back.
inline constexpr uint64_t kIoOpFollowMaxNanos = 200'000;

// What a report covers of one step: how many, their total, the longest.
struct IoStepSpan {
    uint64_t count{0};
    uint64_t ticks{0};
    uint64_t maxTicks{0};
};

struct IoStepTiming {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> totalTicks{0};
    std::atomic<uint64_t> maxTicks{0};        // since the report last took it
    std::atomic<uint64_t> streamMaxTicks{0};  // since Reset
    // Reader's side: what the last report covered.
    uint64_t reportedCount{0};
    uint64_t reportedTicks{0};

    // IO thread.
    void Record(uint64_t ticks) noexcept {
        count.fetch_add(1, std::memory_order_relaxed);
        totalTicks.fetch_add(ticks, std::memory_order_relaxed);
        if (ticks > maxTicks.load(std::memory_order_relaxed)) {
            maxTicks.store(ticks, std::memory_order_relaxed);
        }
        if (ticks > streamMaxTicks.load(std::memory_order_relaxed)) {
            streamMaxTicks.store(ticks, std::memory_order_relaxed);
        }
    }

    // Reader: what was recorded since the last Take.
    [[nodiscard]] IoStepSpan Take() noexcept {
        const uint64_t nowCount = count.load(std::memory_order_relaxed);
        const uint64_t nowTicks = totalTicks.load(std::memory_order_relaxed);
        const IoStepSpan span{
            .count = nowCount - reportedCount,
            .ticks = nowTicks - reportedTicks,
            .maxTicks = maxTicks.exchange(0, std::memory_order_relaxed),
        };
        reportedCount = nowCount;
        reportedTicks = nowTicks;
        return span;
    }

    // Reader: everything since Reset.
    [[nodiscard]] IoStepSpan Stream() const noexcept {
        return {
            .count = count.load(std::memory_order_relaxed),
            .ticks = totalTicks.load(std::memory_order_relaxed),
            .maxTicks = streamMaxTicks.load(std::memory_order_relaxed),
        };
    }

    void Reset() noexcept {
        count.store(0, std::memory_order_relaxed);
        totalTicks.store(0, std::memory_order_relaxed);
        maxTicks.store(0, std::memory_order_relaxed);
        streamMaxTicks.store(0, std::memory_order_relaxed);
        reportedCount = 0;
        reportedTicks = 0;
    }
};

struct IoOpTiming {
    IoStepTiming beginRead;
    IoStepTiming writeEnd;
    IoStepTiming pull;    // within BeginRead
    IoStepTiming fill;    // within WriteEnd
    IoStepTiming follow;  // from another device's same operation returning to this one's entry
    std::atomic<uint64_t> followMinTicks{UINT64_MAX};  // since Reset
    // Operations that took slowTicks or longer: how many, and the latest.
    std::atomic<uint64_t> slow{0};
    IoOpTrace slowOps;
    // The last fill's time, for the WriteEnd that ran it (0: it filled nothing).
    std::atomic<uint64_t> lastFillTicks{0};
    // kSlowIoOpNanos and kIoOpFollowMaxNanos in host ticks (Reset).
    uint64_t slowTicks{UINT64_MAX};
    uint64_t followMaxTicks{0};
    // Reader's side: when the stream started, what the last report covered,
    // when it went out and when the next one is due.
    uint64_t streamStartTicks{0};
    uint64_t reportedSlow{0};
    uint64_t reportedSlowOps{0};
    uint64_t reportedPullWaits{0};
    uint64_t reportedPullBusy{0};
    uint64_t streamStartPullWaits{0};
    uint64_t streamStartPullBusy{0};
    uint64_t lastReportTicks{0};
    uint64_t nextReportTicks{0};

    // IO thread: an operation that took slowTicks or longer.
    void RecordSlow(const IoOp& op) noexcept {
        slow.fetch_add(1, std::memory_order_relaxed);
        slowOps.Record(op);
    }

    // IO thread.
    void RecordFollow(uint64_t ticks) noexcept {
        if (ticks >= followMaxTicks) {
            return;
        }
        follow.Record(ticks);
        if (ticks < followMinTicks.load(std::memory_order_relaxed)) {
            followMinTicks.store(ticks, std::memory_order_relaxed);
        }
    }

    // A new stream, before IO runs. The pull counters (RxPullHook) run on
    // across streams: `pullWaits` and `pullBusy` are where they stand now.
    void Reset(uint64_t nowTicks, uint64_t slowThresholdTicks, uint64_t followLimitTicks,
               uint64_t pullWaits, uint64_t pullBusy) noexcept {
        beginRead.Reset();
        writeEnd.Reset();
        pull.Reset();
        fill.Reset();
        follow.Reset();
        followMinTicks.store(UINT64_MAX, std::memory_order_relaxed);
        slow.store(0, std::memory_order_relaxed);
        lastFillTicks.store(0, std::memory_order_relaxed);
        slowTicks = slowThresholdTicks != 0 ? slowThresholdTicks : UINT64_MAX;
        followMaxTicks = followLimitTicks;
        reportedSlow = 0;
        reportedSlowOps = slowOps.Recorded();
        reportedPullWaits = pullWaits;
        reportedPullBusy = pullBusy;
        streamStartPullWaits = pullWaits;
        streamStartPullBusy = pullBusy;
        streamStartTicks = nowTicks;
        lastReportTicks = nowTicks;
        nextReportTicks = 0;
    }
};

// The operation that returned last in this process, on any device: when, and
// which device and operation (IoOpFollowTag). Two words written one after the
// other by the IO thread; a reader that catches them mid-update takes a gap
// between unrelated operations, which the device and operation test and
// kIoOpFollowMaxNanos keep out of the figures in all but a few cases.
struct IoOpLastReturn {
    std::atomic<uint64_t> ticks{0};
    std::atomic<uintptr_t> tag{0};
};

// Device and operation in one word: `device` is at least 2-aligned.
[[nodiscard]] inline uintptr_t IoOpFollowTag(const void* device, bool writeEnd) noexcept {
    return reinterpret_cast<uintptr_t>(device) | (writeEnd ? 1U : 0U);
}

// True when `previousTag` is the same operation on another device.
[[nodiscard]] inline bool IoOpFollows(uintptr_t previousTag, const void* device,
                                      bool writeEnd) noexcept {
    return previousTag != 0 && ((previousTag & 1U) != 0) == writeEnd &&
           (previousTag & ~static_cast<uintptr_t>(1)) != reinterpret_cast<uintptr_t>(device);
}

} // namespace ASFW::Audio::Runtime
