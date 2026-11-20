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
// And when CoreAudio got here: BeginRead's entry after the host time CoreAudio
// gives for the read's first frame, WriteEnd's entry before the host time it
// gives for the write's first frame (its presentation), and "render": from
// this device's BeginRead returning to its WriteEnd beginning, which is the
// client's IO procedure with CoreAudio's way out of one handler and into the
// next (and, in an aggregate, the other units' operations). Render is also
// kept by position in a run of kRenderPhaseFrames written frames: a client
// that works in blocks of that many frames, or a divisor of it, shows as one
// position whose mean stands out.
//
// Written by the IO thread, read by the [IoTime] report on the transmit
// producer's thread: counts and totals only grow (the report keeps what it
// has printed), the longest of an interval is taken with an exchange.
//

#pragma once

#include "GlitchEventLog.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// An operation this long is reported at warn ([IoTime], [IoSlow]).
inline constexpr uint64_t kSlowIoOpNanos = 50'000;
// A gap this long is the IO cycle itself, not two operations back to back.
inline constexpr uint64_t kIoOpFollowMaxNanos = 200'000;

// Render by position: a run of kRenderPhaseFrames written frames in bins of
// kRenderPhaseBinFrames (8192 = 128 cycles of 64 frames; half a cycle at the
// smallest buffer in use).
inline constexpr uint32_t kRenderPhaseFrames = 8192;
inline constexpr uint32_t kRenderPhaseBinFrames = 32;
inline constexpr uint32_t kRenderPhaseBins = kRenderPhaseFrames / kRenderPhaseBinFrames;

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

// A signed distance in host ticks between an operation's entry and a host
// time CoreAudio gave with it.
struct IoOffsetSpan {
    uint64_t count{0};
    int64_t ticks{0};
    int64_t minTicks{0};
    int64_t maxTicks{0};
};

struct IoOffsetTiming {
    std::atomic<uint64_t> count{0};
    std::atomic<int64_t> totalTicks{0};
    std::atomic<int64_t> minTicks{INT64_MAX};  // since the report last took them
    std::atomic<int64_t> maxTicks{INT64_MIN};
    std::atomic<int64_t> streamMinTicks{INT64_MAX};  // since Reset
    std::atomic<int64_t> streamMaxTicks{INT64_MIN};
    // Reader's side: what the last report covered.
    uint64_t reportedCount{0};
    int64_t reportedTicks{0};

    // IO thread.
    void Record(int64_t ticks) noexcept {
        count.fetch_add(1, std::memory_order_relaxed);
        totalTicks.fetch_add(ticks, std::memory_order_relaxed);
        if (ticks < minTicks.load(std::memory_order_relaxed)) {
            minTicks.store(ticks, std::memory_order_relaxed);
            if (ticks < streamMinTicks.load(std::memory_order_relaxed)) {
                streamMinTicks.store(ticks, std::memory_order_relaxed);
            }
        }
        if (ticks > maxTicks.load(std::memory_order_relaxed)) {
            maxTicks.store(ticks, std::memory_order_relaxed);
            if (ticks > streamMaxTicks.load(std::memory_order_relaxed)) {
                streamMaxTicks.store(ticks, std::memory_order_relaxed);
            }
        }
    }

    // Reader: what was recorded since the last Take.
    [[nodiscard]] IoOffsetSpan Take() noexcept {
        const uint64_t nowCount = count.load(std::memory_order_relaxed);
        const int64_t nowTicks = totalTicks.load(std::memory_order_relaxed);
        IoOffsetSpan span{.count = nowCount - reportedCount, .ticks = nowTicks - reportedTicks};
        const int64_t least = minTicks.exchange(INT64_MAX, std::memory_order_relaxed);
        const int64_t most = maxTicks.exchange(INT64_MIN, std::memory_order_relaxed);
        if (span.count != 0 && least <= most) {
            span.minTicks = least;
            span.maxTicks = most;
        } else {
            span.count = 0;
        }
        reportedCount = nowCount;
        reportedTicks = nowTicks;
        return span;
    }

    // Reader: everything since Reset.
    [[nodiscard]] IoOffsetSpan Stream() const noexcept {
        IoOffsetSpan span{.count = count.load(std::memory_order_relaxed),
                          .ticks = totalTicks.load(std::memory_order_relaxed)};
        const int64_t least = streamMinTicks.load(std::memory_order_relaxed);
        const int64_t most = streamMaxTicks.load(std::memory_order_relaxed);
        if (span.count != 0 && least <= most) {
            span.minTicks = least;
            span.maxTicks = most;
        } else {
            span.count = 0;
        }
        return span;
    }

    void Reset() noexcept {
        count.store(0, std::memory_order_relaxed);
        totalTicks.store(0, std::memory_order_relaxed);
        minTicks.store(INT64_MAX, std::memory_order_relaxed);
        maxTicks.store(INT64_MIN, std::memory_order_relaxed);
        streamMinTicks.store(INT64_MAX, std::memory_order_relaxed);
        streamMaxTicks.store(INT64_MIN, std::memory_order_relaxed);
        reportedCount = 0;
        reportedTicks = 0;
    }
};

// What a report covers of render by position: the positions with the highest
// and the lowest mean, among those with at least a quarter of the busiest
// position's renders (a position a few stray cycles fell on says nothing).
struct IoRenderPhaseSpan {
    uint32_t positions{0};  // with renders at all
    uint32_t highFrame{0};
    uint64_t highCount{0};
    uint64_t highTicks{0};  // total
    uint32_t lowFrame{0};
    uint64_t lowCount{0};
    uint64_t lowTicks{0};
};

struct IoRenderPhases {
    struct Bin {
        std::atomic<uint64_t> ticks{0};
        std::atomic<uint32_t> count{0};
    };
    std::array<Bin, kRenderPhaseBins> bins{};
    // Reader's side: what the last report covered.
    std::array<uint64_t, kRenderPhaseBins> reportedTicks{};
    std::array<uint32_t, kRenderPhaseBins> reportedCount{};

    // IO thread: a render that began with `writtenFrames` frames written.
    void Record(uint64_t writtenFrames, uint64_t ticks) noexcept {
        Bin& bin = bins[(writtenFrames % kRenderPhaseFrames) / kRenderPhaseBinFrames];
        bin.ticks.fetch_add(ticks, std::memory_order_relaxed);
        bin.count.fetch_add(1, std::memory_order_relaxed);
    }

    // Reader: since the last Take (`take`), or everything since Reset.
    [[nodiscard]] IoRenderPhaseSpan Span(bool take) noexcept {
        std::array<uint64_t, kRenderPhaseBins> ticks{};
        std::array<uint32_t, kRenderPhaseBins> count{};
        uint32_t busiest = 0;
        for (uint32_t index = 0; index < kRenderPhaseBins; ++index) {
            const uint32_t nowCount = bins[index].count.load(std::memory_order_relaxed);
            const uint64_t nowTicks = bins[index].ticks.load(std::memory_order_relaxed);
            count[index] = take ? nowCount - reportedCount[index] : nowCount;
            ticks[index] = take ? nowTicks - reportedTicks[index] : nowTicks;
            if (take) {
                reportedCount[index] = nowCount;
                reportedTicks[index] = nowTicks;
            }
            busiest = count[index] > busiest ? count[index] : busiest;
        }
        IoRenderPhaseSpan span{};
        const uint32_t enough = busiest / 4 != 0 ? busiest / 4 : 1;
        for (uint32_t index = 0; index < kRenderPhaseBins; ++index) {
            if (count[index] == 0) {
                continue;
            }
            ++span.positions;
            if (count[index] < enough) {
                continue;
            }
            const uint64_t mean = ticks[index] / count[index];
            if (span.highCount == 0 || mean > span.highTicks / span.highCount) {
                span.highFrame = index * kRenderPhaseBinFrames;
                span.highCount = count[index];
                span.highTicks = ticks[index];
            }
            if (span.lowCount == 0 || mean < span.lowTicks / span.lowCount) {
                span.lowFrame = index * kRenderPhaseBinFrames;
                span.lowCount = count[index];
                span.lowTicks = ticks[index];
            }
        }
        return span;
    }

    void Reset() noexcept {
        for (uint32_t index = 0; index < kRenderPhaseBins; ++index) {
            bins[index].ticks.store(0, std::memory_order_relaxed);
            bins[index].count.store(0, std::memory_order_relaxed);
        }
        reportedTicks.fill(0);
        reportedCount.fill(0);
    }
};

struct IoOpTiming {
    IoStepTiming beginRead;
    IoStepTiming writeEnd;
    IoStepTiming pull;    // within BeginRead
    IoStepTiming fill;    // within WriteEnd
    IoStepTiming follow;  // from another device's same operation returning to this one's entry
    std::atomic<uint64_t> followMinTicks{UINT64_MAX};  // since Reset
    // BeginRead's entry after CoreAudio's host time for its first frame;
    // CoreAudio's host time for a WriteEnd's first frame after its entry.
    IoOffsetTiming readArrival;
    IoOffsetTiming writeMargin;
    // From BeginRead's return to WriteEnd's entry, and the same by position.
    IoStepTiming render;
    IoRenderPhases renderPhases;
    // The last BeginRead's return, until its WriteEnd takes it.
    std::atomic<uint64_t> readReturnTicks{0};
    std::atomic<uint64_t> writtenFrames{0};  // by the WriteEnds since Reset
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

    // IO thread: an operation began at `entryTicks`; `hostTicks` is the host
    // time CoreAudio gave with it (0: none).
    void RecordArrival(bool isWriteEnd, uint64_t entryTicks, uint64_t hostTicks) noexcept {
        if (hostTicks == 0) {
            return;
        }
        if (isWriteEnd) {
            writeMargin.Record(static_cast<int64_t>(hostTicks - entryTicks));
        } else {
            readArrival.Record(static_cast<int64_t>(entryTicks - hostTicks));
        }
    }

    // IO thread: BeginRead returned.
    void RecordReadReturn(uint64_t returnTicks) noexcept {
        readReturnTicks.store(returnTicks, std::memory_order_relaxed);
    }

    // IO thread: a WriteEnd of `frames` frames began. The render before it
    // counts when a BeginRead of this device returned since the last WriteEnd.
    void RecordWriteEntry(uint64_t entryTicks, uint32_t frames) noexcept {
        const uint64_t returned = readReturnTicks.exchange(0, std::memory_order_relaxed);
        const uint64_t written = writtenFrames.fetch_add(frames, std::memory_order_relaxed);
        if (returned == 0 || entryTicks <= returned) {
            return;
        }
        render.Record(entryTicks - returned);
        renderPhases.Record(written, entryTicks - returned);
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
        readArrival.Reset();
        writeMargin.Reset();
        render.Reset();
        renderPhases.Reset();
        readReturnTicks.store(0, std::memory_order_relaxed);
        writtenFrames.store(0, std::memory_order_relaxed);
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
