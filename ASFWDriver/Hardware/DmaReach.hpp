//
// DmaReach.hpp
// ASFWDriver
//
// Which bus addresses the OHCI controller can use for DMA.
//
// The controller's DMA engines take 32-bit addresses: its descriptors and DMA
// address registers have 32-bit address fields. Without an
// IOMMU a page's bus address is its physical address, so on a machine with more
// than 4 GiB of RAM most pages are out of reach; xnu then routes a 32-bit
// IODMACommand through a copy buffer that the controller and the driver never
// share (b55). A page is also out of reach when the address is claimed on the
// way to the root complex: under passthrough without a virtual IOMMU the guest
// programs guest-physical addresses, and a PCIe-to-PCI bridge keeps any address
// inside its own downstream window for its secondary bus (an ESXi host placed
// the XIO2213 bridge's window at 0x9030_0000-0x903F_FFFF, which is RAM in a
// 3 GiB guest; every IT packet read from there ended evt_data_read, b170).
// HardwareInterface builds DMA buffers from pages that pass Reachable() when
// ordinary allocation would bounce, or when the controller's provider names
// excluded ranges in the asfw-dma-exclude property.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ASFW::Driver::DmaReach {

/// First bus address the controller cannot reach.
inline constexpr uint64_t kAddressLimit = uint64_t{1} << 32;

/// IOMemoryDescriptor::CreateWithMemoryDescriptors takes at most 32 descriptors
/// (IOMemoryDescriptor.iig); one level of nesting gives 32 × 32 pages.
inline constexpr size_t kDescriptorsPerComposite = 32;
inline constexpr size_t kMaxCompositePages = kDescriptorsPerComposite * kDescriptorsPerComposite;

/// An inclusive bus-address range.
struct BusRange {
    uint64_t first{0};
    uint64_t last{0};
};

/// The range in `excluded` that [address, address + length) overlaps, or nullptr.
[[nodiscard]] inline const BusRange* ExcludedHit(uint64_t address, uint64_t length,
                                                 std::span<const BusRange> excluded) noexcept {
    if (length == 0) {
        return nullptr;
    }
    const uint64_t last = address + length - 1;
    for (const auto& range : excluded) {
        if (address <= range.last && last >= range.first) {
            return &range;
        }
    }
    return nullptr;
}

/// True when the controller can reach every byte of [address, address + length).
[[nodiscard]] inline bool Reachable(uint64_t address, uint64_t length,
                                    std::span<const BusRange> excluded) noexcept {
    if (length == 0 || address >= kAddressLimit || length > kAddressLimit - address) {
        return false;
    }
    return ExcludedHit(address, length, excluded) == nullptr;
}

namespace Detail {

[[nodiscard]] inline bool ParseHex(std::string_view text, uint64_t& value) noexcept {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 16) {
        return false;
    }
    value = 0;
    for (const char c : text) {
        uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint64_t>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    return true;
}

[[nodiscard]] constexpr bool IsSeparator(char c) noexcept {
    return c == ',' || c == ' ' || c == ';' || c == '\t' || c == '\n';
}

} // namespace Detail

/// Parses "0x90300000-0x903fffff[, first-last ...]" (inclusive, hexadecimal).
/// Returns false, and leaves `out` empty, on any malformed entry.
[[nodiscard]] inline bool ParseExcludedRanges(std::string_view text, std::vector<BusRange>& out) {
    out.clear();
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && Detail::IsSeparator(text[position])) {
            ++position;
        }
        if (position >= text.size()) {
            break;
        }
        size_t end = position;
        while (end < text.size() && !Detail::IsSeparator(text[end])) {
            ++end;
        }
        const std::string_view entry = text.substr(position, end - position);
        const size_t dash = entry.find('-');
        BusRange range{};
        if (dash == std::string_view::npos || !Detail::ParseHex(entry.substr(0, dash), range.first) ||
            !Detail::ParseHex(entry.substr(dash + 1), range.last) || range.last < range.first) {
            out.clear();
            return false;
        }
        out.push_back(range);
        position = end;
    }
    return true;
}

} // namespace ASFW::Driver::DmaReach
