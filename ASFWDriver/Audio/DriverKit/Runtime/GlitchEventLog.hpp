//
// GlitchEventLog.hpp
// ASFWDriver
//
// Lost audio with its exact place in the stream (b187 report §5). Every event
// carries CoreAudio's sample time for the device, the 64-bit running frame
// count since the stream started that a client's IOProc sees in its
// AudioTimeStamp.mSampleTime: an event lines up one to one with an offset in
// a recording, and two units' events with each other.
//
// Written on AudioDriverKit's IO threads (BeginRead and WriteEnd; there is one
// per client connection, so two can write at once), read by the [Glitch]
// report on the TX preparation queue. A writer that laps the reader overwrites
// the oldest event, and the reader counts what it lost.
//
// IoOpTrace (debug level only): the last IO operations on the device, any
// thread, and one capture of them taken at a read ahead of the arrivals, so
// the report can show what the HAL did around it (b190 report §1: with two
// clients on a unit, a BeginRead at the output position follows the input
// read by microseconds, and the thread tags did not tell the clients apart).
//

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class GlitchEventKind : uint8_t {
    ShortCaptureRead = 1,    // a BeginRead found frames missing
    MissedOutputFrames = 2,  // output frames reached their packet too late
    // A BeginRead whose first frame lies at or past the capture ring's write
    // position after start-up: none of it has arrived, so no pull can serve
    // it. b190 saw these only with two clients on a unit, at the output
    // write's position (2 x buffer + the safety offsets past the input read),
    // microseconds after a complete input read. Kept apart from short reads:
    // they are not S_in headroom, and the frames read are not input.
    ReadAhead = 3,
};

// What a BeginRead pull found (RxPullHook::Pull).
enum class RxPullOutcome : uint8_t {
    NoPull = 0,      // nothing to pull from: no receive context published
    Covered = 1,     // the capture ring covers the read
    NotArrived = 2,  // drained every completed packet; the rest had not arrived
    Cut = 3,         // its own drain stopped at the deadline
    Busy = 4,        // another poll held the receive context past the deadline
};

// The fields mean one thing for a read (ShortCaptureRead, ReadAhead) and
// another for MissedOutputFrames, noted as read / output.
struct GlitchEvent {
    uint64_t sampleFrame{0};  // the read's first frame / the first missed output frame
    uint64_t hostTicks{0};    // mach_absolute_time when the driver saw it
    GlitchEventKind kind{GlitchEventKind::ShortCaptureRead};
    RxPullOutcome pull{RxPullOutcome::NoPull};
    uint16_t frames{0};       // the read's frame count / output frames missed
    uint16_t missing{0};      // read frames past the capture write position after the pull / the write's frames
    uint16_t drained{0};      // packets the pull drained / packets sent but not yet harvested by a refill
    uint16_t thread{0};       // which IO thread (CurrentThreadTag)
    int32_t gapBefore{0};     // frames past the capture write position before the pull / packets the write's first frame lay behind the finality frontier
    int32_t leadUs{0};        // BeginRead time minus the read's last frame's time, by our zero timestamps / the HAL's presentation time for the write's first frame minus WriteEnd entry
    int32_t skewUs{0};        // CoreAudio's host time for the read's first frame minus ours / the fill's duration
    int32_t prevFrames{0};    // this read's first frame minus the previous BeginRead's on this device / 0
    int32_t prevUs{0};        // time since that BeginRead / age of the transmit clock pair (the last refill)
    uint32_t ztsAgeUs{0};     // since the latest zero timestamp went to CoreAudio / 0
};

// A tag for the calling thread: the page of its stack frame. Taken once per
// IO operation at the top of the IO handler, where every operation on a thread
// has the same frame address, so a thread's tag is stable across BeginRead and
// WriteEnd. Thread stacks are 512 KiB apart, so two IO threads' pages differ
// (b190 used the 1 MiB region, which two adjacent stacks shared).
[[nodiscard]] inline uint16_t CurrentThreadTag() noexcept {
    return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) >> 12);
}

class GlitchEventLog {
public:
    static constexpr uint32_t kCapacity = 64;

    // Any IO thread.
    void Record(const GlitchEvent& event) noexcept {
        const uint64_t index = head_.fetch_add(1, std::memory_order_relaxed);
        Slot& slot = slots_[index % kCapacity];
        slot.sequence.store(index * 2 + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.event = event;
        slot.sequence.store(index * 2 + 2, std::memory_order_release);
    }

    // The one reader. Copies up to `max` events not read yet, oldest first,
    // and returns how many. `lost` grows by the events overwritten before
    // they were read. An event still being written ends the take; the next
    // one starts there.
    [[nodiscard]] uint32_t Take(GlitchEvent* out, uint32_t max, uint64_t& lost) noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (head - tail_ > kCapacity) {
            lost += head - tail_ - kCapacity;
            tail_ = head - kCapacity;
        }
        uint32_t taken = 0;
        while (tail_ < head && taken < max) {
            Slot& slot = slots_[tail_ % kCapacity];
            const uint64_t want = tail_ * 2 + 2;
            const uint64_t before = slot.sequence.load(std::memory_order_acquire);
            if (before < want) {
                break;  // claimed, not written yet
            }
            GlitchEvent copy = slot.event;
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t after = slot.sequence.load(std::memory_order_relaxed);
            ++tail_;
            if (before != want || after != want) {
                ++lost;  // overwritten by a writer a lap ahead
                continue;
            }
            out[taken++] = copy;
        }
        return taken;
    }

    // Events recorded and not taken yet (reader).
    [[nodiscard]] uint64_t Pending() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_;
    }

    // Per stream start, on the reader's side, before IO runs.
    void Reset() noexcept { tail_ = head_.load(std::memory_order_acquire); }

private:
    struct Slot {
        std::atomic<uint64_t> sequence{0};
        GlitchEvent event{};
    };
    std::array<Slot, kCapacity> slots_{};
    std::atomic<uint64_t> head_{0};
    uint64_t tail_{0};
};

// One IO operation as the HAL asked for it: every operation at debug level
// (IoOpTrace), the slow ones at any level (IoOpTiming.hpp).
struct IoOp {
    uint64_t entryTicks{0};   // mach_absolute_time at the IO handler's entry
    uint64_t sampleTime{0};   // the HAL's sample time for the operation
    uint64_t hostTime{0};     // the HAL's host time for that sample time
    uint32_t frames{0};
    uint32_t durationNanos{0};  // the handler's time
    uint32_t partNanos{0};      // of it, a BeginRead's pull / a WriteEnd's fill
    uint16_t operation{0};    // IOUserAudioIOOperation: 0 BeginRead, 1 WriteEnd
    uint16_t thread{0};       // CurrentThreadTag
};

// The last kIoOpTraceCapacity operations on a device, written by any IO thread
// (per-slot sequence numbers as in GlitchEventLog), copied out oldest first
// by Snapshot without consuming them.
class IoOpTrace {
public:
    static constexpr uint32_t kCapacity = 16;

    void Record(const IoOp& op) noexcept {
        const uint64_t index = head_.fetch_add(1, std::memory_order_relaxed);
        Slot& slot = slots_[index % kCapacity];
        slot.sequence.store(index * 2 + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.op = op;
        slot.sequence.store(index * 2 + 2, std::memory_order_release);
    }

    // The newest `max` operations, oldest first, skipping a slot a writer
    // holds or has overwritten meanwhile. Returns how many were copied.
    [[nodiscard]] uint32_t Snapshot(IoOp* out, uint32_t max) const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        const uint64_t available = head < kCapacity ? head : kCapacity;
        const uint64_t wanted = available < max ? available : max;
        uint32_t copied = 0;
        for (uint64_t index = head - wanted; index < head; ++index) {
            const Slot& slot = slots_[index % kCapacity];
            const uint64_t want = index * 2 + 2;
            if (slot.sequence.load(std::memory_order_acquire) != want) {
                continue;
            }
            IoOp copy = slot.op;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.sequence.load(std::memory_order_relaxed) != want) {
                continue;
            }
            out[copied++] = copy;
        }
        return copied;
    }

    [[nodiscard]] uint64_t Recorded() const noexcept { return head_.load(std::memory_order_acquire); }

private:
    struct Slot {
        std::atomic<uint64_t> sequence{0};
        IoOp op{};
    };
    std::array<Slot, kCapacity> slots_{};
    std::atomic<uint64_t> head_{0};
};

// One capture of the trace, taken by the IO thread that found a read ahead
// (TryCapture, at most one until the report takes it) and printed by the
// [Glitch] report (Take). The read itself is the last operation captured.
class IoOpTraceCapture {
public:
    static constexpr uint32_t kMaxOps = IoOpTrace::kCapacity;

    // Any IO thread. False when a capture is already waiting.
    bool TryCapture(const IoOpTrace& trace, const IoOp& last) noexcept {
        uint8_t expected = kEmpty;
        if (!state_.compare_exchange_strong(expected, kWriting, std::memory_order_acq_rel)) {
            return false;
        }
        count_ = trace.Snapshot(ops_.data(), kMaxOps - 1);
        ops_[count_++] = last;
        state_.store(kReady, std::memory_order_release);
        return true;
    }

    // The reader: copies the capture out and frees the slot. 0 when none waits.
    [[nodiscard]] uint32_t Take(IoOp* out, uint32_t max) noexcept {
        if (state_.load(std::memory_order_acquire) != kReady) {
            return 0;
        }
        const uint32_t count = count_ < max ? count_ : max;
        for (uint32_t index = 0; index < count; ++index) {
            out[index] = ops_[index];
        }
        state_.store(kEmpty, std::memory_order_release);
        return count;
    }

    void Reset() noexcept { state_.store(kEmpty, std::memory_order_release); }

private:
    static constexpr uint8_t kEmpty = 0;
    static constexpr uint8_t kWriting = 1;
    static constexpr uint8_t kReady = 2;
    std::atomic<uint8_t> state_{kEmpty};
    uint32_t count_{0};
    std::array<IoOp, kMaxOps> ops_{};
};

} // namespace ASFW::Audio::Runtime
