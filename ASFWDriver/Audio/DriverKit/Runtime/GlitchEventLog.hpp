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

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class GlitchEventKind : uint8_t {
    ShortCaptureRead = 1,    // a BeginRead found frames missing
    MissedOutputFrames = 2,  // output frames reached their packet too late
};

// What a BeginRead pull found (RxPullHook::Pull).
enum class RxPullOutcome : uint8_t {
    NoPull = 0,      // nothing to pull from: no receive context published
    Covered = 1,     // the capture ring covers the read
    NotArrived = 2,  // drained every completed packet; the rest had not arrived
    Cut = 3,         // its own drain stopped at the deadline
    Busy = 4,        // another poll held the receive context past the deadline
};

struct GlitchEvent {
    uint64_t sampleFrame{0};  // the read's first frame, or the first missed output frame
    uint64_t hostTicks{0};    // mach_absolute_time when the driver saw it
    GlitchEventKind kind{GlitchEventKind::ShortCaptureRead};
    RxPullOutcome pull{RxPullOutcome::NoPull};
    uint16_t frames{0};       // the read's frame count, or output frames missed
    uint16_t missing{0};      // read frames past the capture write position after the pull
    uint16_t drained{0};      // packets the pull drained
    uint16_t thread{0};       // which IO thread (a tag of its stack region)
    int32_t gapBefore{0};     // frames past the capture write position before the pull
    int32_t leadUs{0};        // BeginRead time minus the read's last frame's time, by our zero timestamps
    int32_t skewUs{0};        // CoreAudio's host time for the read's first frame minus ours
    int32_t prevFrames{0};    // this read's first frame minus the previous BeginRead's on this device
    int32_t prevUs{0};        // time since that BeginRead
    uint32_t ztsAgeUs{0};     // since the latest zero timestamp went to CoreAudio
};

// A tag for the calling thread: its stack region (the frame address over
// 1 MiB). The IO threads of two client connections have stacks far apart, so
// their tags differ; enough to tell which one recorded an event.
[[nodiscard]] inline uint16_t CurrentThreadTag() noexcept {
    return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) >> 20);
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

} // namespace ASFW::Audio::Runtime
