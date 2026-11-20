#pragma once

#include "AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

class PcmSlotCodec final {
public:
    // Float [-1.0, +1.0] to signed 24-bit: symmetric scale by 2^23-1, round
    // half away from zero. Inline: the payload writers call it per sample on
    // the IO thread.
    [[nodiscard]] static int32_t Float32ToSigned24(float sample) noexcept {
        constexpr float kScale24 = 8388607.0f; // 2^23 - 1
        if (sample > 1.0f) {
            sample = 1.0f;
        } else if (sample < -1.0f) {
            sample = -1.0f;
        }
        const float scaled = sample * kScale24;
        return (scaled >= 0.0f) ? static_cast<int32_t>(scaled + 0.5f)
                                : static_cast<int32_t>(scaled - 0.5f);
    }
    [[nodiscard]] static int32_t Float32ToSigned32(float sample) noexcept;

    [[nodiscard]] static uint32_t EncodeFloat32(float sample,
                                                PcmSlotEncoding encoding) noexcept;
    [[nodiscard]] static uint32_t EncodeInt32(int32_t sample,
                                              PcmSlotEncoding encoding) noexcept;

    static void EncodeInterleavedFloat32Frame(const float* sourceFrame,
                                              uint32_t sourceChannels,
                                              uint32_t* destinationSlots,
                                              uint32_t destinationSlotCount,
                                              uint32_t pcmChannels,
                                              PcmSlotEncoding encoding) noexcept;

private:
    [[nodiscard]] static uint32_t EncodeAm824MBLA(float sample) noexcept;
    [[nodiscard]] static uint32_t EncodeRawSigned24In32BE(float sample) noexcept;
    [[nodiscard]] static uint32_t EncodeRawSigned24In32LE(float sample) noexcept;
    [[nodiscard]] static uint32_t EncodeRawPcm24Upper24In32LE(float sample) noexcept;
    [[nodiscard]] static int32_t NormalizeSigned24(int32_t sample) noexcept;
};

} // namespace ASFW::Protocols::Audio::AMDTP
