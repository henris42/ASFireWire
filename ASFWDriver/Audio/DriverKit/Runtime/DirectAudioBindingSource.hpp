#pragma once

#include "AudioTransportControlBlock.hpp"

#include <cstdint>

namespace ASFW::Audio::Runtime {

struct DirectAudioBindingSnapshot {
    uint64_t generation{0};

    float* inputBase{nullptr};
    uint64_t inputBytes{0};
    uint32_t inputFrames{0};
    uint32_t inputChannels{0};

    const float* outputBase{nullptr};
    uint64_t outputBytes{0};
    uint32_t outputFrames{0};
    uint32_t outputChannels{0};

    AudioTransportControlBlock* control{nullptr};
    uint32_t sampleRateHz{0};
    bool valid{false};

    [[nodiscard]] bool HasInput() const noexcept {
        return inputBase != nullptr && inputFrames > 0 && inputChannels > 0;
    }

    [[nodiscard]] bool HasOutput() const noexcept {
        return outputBase != nullptr && outputFrames > 0 && outputChannels > 0;
    }

    [[nodiscard]] bool IsValidDuplex() const noexcept {
        return HasInput() && HasOutput() && control != nullptr && sampleRateHz > 0 && valid;
    }
};

class IDirectAudioBindingSource {
public:
    virtual ~IDirectAudioBindingSource() = default;
    [[nodiscard]] virtual bool IsTransmitTimingReady() noexcept { return false; }
    // The unit's sample rate as its own timestamps measure it, once enough of
    // the current stream has arrived. False before that, and for a family whose
    // receive side publishes no device clock.
    [[nodiscard]] virtual bool MeasuredDeviceRateHz(uint32_t& outRateHz) noexcept {
        (void)outRateHz;
        return false;
    }
    [[nodiscard]] virtual bool CopyDirectAudioBinding(DirectAudioBindingSnapshot& out) noexcept = 0;
    // The generation CopyDirectAudioBinding would report now, read without
    // its lock: 0 while there is no complete binding. The receive path checks
    // it on every batch and copies only when it changed, so a batch takes no
    // lock. A source that cannot tell returns kBindingGenerationUnknown, and
    // the caller copies.
    static constexpr uint64_t kBindingGenerationUnknown = UINT64_MAX;
    [[nodiscard]] virtual uint64_t PeekDirectAudioBindingGeneration() const noexcept {
        return kBindingGenerationUnknown;
    }
    // Our 6-bit node id for the CIP header's SID, from the session when it
    // prepares the transmit context. Stored in the control block for the
    // producer; a source without one ignores it.
    virtual void PublishCipSourceId(uint8_t sid) noexcept { (void)sid; }
};

} // namespace ASFW::Audio::Runtime
