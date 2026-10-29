#pragma once

#include <algorithm>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct PlaybackRingRangeUpdate final {
    uint64_t oldestValidFrame{0};
    uint64_t writtenEndFrame{0};
    bool discontinuity{false};
    bool overrun{false};
    // The write ended behind the recorded written end: that end ran ahead of
    // what CoreAudio has written, and the range followed the write back.
    bool rewound{false};
};

[[nodiscard]] constexpr PlaybackRingRangeUpdate
UpdatePlaybackRingRange(uint64_t previousWrittenEnd,
                        uint64_t previousOldestValid,
                        uint64_t writeStart,
                        uint64_t writeEnd,
                        uint64_t consumedEnd,
                        uint32_t capacityFrames) noexcept {
    PlaybackRingRangeUpdate result{
        .oldestValidFrame = previousOldestValid,
        .writtenEndFrame = previousWrittenEnd,
    };
    if (writeEnd <= writeStart || writeEnd == previousWrittenEnd) {
        return result;
    }

    if (writeEnd < previousWrittenEnd) {
        // CoreAudio wrote behind the end recorded so far, so that end was never
        // written: only this write's end is known to hold its frames. Holding
        // the old end let the fill copy whatever the ring kept from the last
        // session for every frame up to it (b146: a cold start replayed about
        // 170 ms of the previous session's output). Follow the write back.
        result.rewound = true;
        result.discontinuity = true;
        result.writtenEndFrame = writeEnd;
        result.oldestValidFrame = std::min(previousOldestValid, writeStart);
        if (capacityFrames != 0 && writeEnd > capacityFrames) {
            result.oldestValidFrame =
                std::max(result.oldestValidFrame, writeEnd - capacityFrames);
        }
        return result;
    }

    const bool firstWrite = previousWrittenEnd == 0;
    result.discontinuity = !firstWrite && writeStart != previousWrittenEnd;
    result.writtenEndFrame = writeEnd;

    if (firstWrite || result.discontinuity) {
        result.oldestValidFrame = writeStart;
    }
    if (capacityFrames != 0 && writeEnd > capacityFrames) {
        result.oldestValidFrame =
            std::max(result.oldestValidFrame, writeEnd - capacityFrames);
    }
    result.overrun = consumedEnd != 0 && consumedEnd < result.oldestValidFrame;
    return result;
}

} // namespace ASFW::Audio::Runtime
