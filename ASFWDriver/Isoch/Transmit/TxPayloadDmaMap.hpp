#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace ASFW::Isoch::Tx {

struct TxPayloadDmaSegment {
    std::uint64_t deviceAddress{0};
    std::uint64_t length{0};
};

struct TxPayloadDmaFragment {
    std::uint32_t deviceAddress{0};
    std::uint32_t length{0};
};

class TxPayloadDmaMap final {
public:
    // The payload slab is ordinary scattered pages (nothing contiguous above
    // 64 KiB is ever requested, b49), so on 4 KiB pages a 2 MB slab is several
    // hundred runs. PrepareForDMA returns at most 32 per call; the context
    // prepares the slab in windows and hands the whole list in here. The cap
    // only bounds a malformed list.
    static constexpr std::size_t kMaxSegments = 8192;

    struct MappedSegment {
        std::uint64_t slabOffset{0};
        std::uint64_t deviceAddress{0};
        std::uint64_t length{0};
    };

    [[nodiscard]] bool Configure(std::span<const TxPayloadDmaSegment> segments,
                                 std::uint64_t slabLength) noexcept {
        Reset();
        if (segments.empty() || segments.size() > kMaxSegments || slabLength == 0) {
            return false;
        }
        segments_.reserve(segments.size());

        std::uint64_t remaining = slabLength;
        std::uint64_t slabOffset = 0;

        for (const auto& segment : segments) {
            if (remaining == 0) {
                break;
            }
            if (segment.deviceAddress == 0 || segment.length == 0) {
                Reset();
                return false;
            }

            const auto mappedLength = segment.length < remaining ? segment.length : remaining;
            constexpr auto kAddressSpaceSize =
                std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1ULL;
            if (segment.deviceAddress >= kAddressSpaceSize ||
                mappedLength > (kAddressSpaceSize - segment.deviceAddress)) {
                Reset();
                return false;
            }

            segments_.push_back(MappedSegment{
                .slabOffset = slabOffset,
                .deviceAddress = segment.deviceAddress,
                .length = mappedLength,
            });
            slabOffset += mappedLength;
            remaining -= mappedLength;
        }

        if (remaining != 0) {
            Reset();
            return false;
        }

        slabLength_ = slabLength;
        return true;
    }

    void Reset() noexcept {
        segments_.clear();
        slabLength_ = 0;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return !segments_.empty() && slabLength_ != 0;
    }

    [[nodiscard]] std::size_t SegmentCount() const noexcept {
        return segments_.size();
    }

    [[nodiscard]] std::uint64_t SlabLength() const noexcept {
        return slabLength_;
    }

    [[nodiscard]] const MappedSegment* SegmentAt(std::size_t index) const noexcept {
        return index < segments_.size() ? &segments_[index] : nullptr;
    }

    /// Index of the run containing `slabOffset`, or SegmentCount() when none does.
    [[nodiscard]] std::size_t SegmentIndexContaining(std::uint64_t slabOffset) const noexcept {
        const MappedSegment* segment = SegmentContaining(slabOffset);
        return segment != nullptr ? static_cast<std::size_t>(segment - segments_.data()) : segments_.size();
    }

    /// The run containing `slabOffset`; runs are in slab order, so a binary
    /// search keeps the per-packet cost flat however many pages the slab has.
    [[nodiscard]] const MappedSegment* SegmentContaining(std::uint64_t slabOffset) const noexcept {
        auto it = std::upper_bound(segments_.begin(), segments_.end(), slabOffset,
                                   [](std::uint64_t value, const MappedSegment& segment) {
                                       return value < segment.slabOffset;
                                   });
        if (it == segments_.begin()) {
            return nullptr;
        }
        --it;
        return slabOffset < it->slabOffset + it->length ? &*it : nullptr;
    }

    [[nodiscard]] bool ResolveTwoFragments(
        std::uint64_t slabOffset,
        std::uint32_t length,
        std::array<TxPayloadDmaFragment, 2>& fragments) const noexcept {
        fragments = {};
        if (length == 0) {
            return true;
        }
        if (!IsValid() || length < 2 || slabOffset >= slabLength_ ||
            std::uint64_t{length} > (slabLength_ - slabOffset)) {
            return false;
        }

        std::uint64_t currentOffset = slabOffset;
        std::uint32_t remaining = length;
        std::size_t fragmentCount = 0;

        while (remaining != 0) {
            const MappedSegment* mapped = SegmentContaining(currentOffset);
            if (!mapped || fragmentCount == fragments.size()) {
                return false;
            }

            const auto segmentOffset = currentOffset - mapped->slabOffset;
            const auto available = mapped->length - segmentOffset;
            const auto fragmentLength =
                std::min<std::uint64_t>(available, remaining);
            fragments[fragmentCount++] = TxPayloadDmaFragment{
                .deviceAddress = static_cast<std::uint32_t>(
                    mapped->deviceAddress + segmentOffset),
                .length = static_cast<std::uint32_t>(fragmentLength),
            };
            currentOffset += fragmentLength;
            remaining -= static_cast<std::uint32_t>(fragmentLength);
        }

        if (fragmentCount == 1) {
            const auto firstLength = fragments[0].length / 2;
            const auto secondLength = fragments[0].length - firstLength;
            fragments[1] = TxPayloadDmaFragment{
                .deviceAddress = fragments[0].deviceAddress + firstLength,
                .length = secondLength,
            };
            fragments[0].length = firstLength;
        }

        return fragments[0].length != 0 && fragments[1].length != 0;
    }

private:
    std::vector<MappedSegment> segments_{};
    std::uint64_t slabLength_{0};
};

} // namespace ASFW::Isoch::Tx
