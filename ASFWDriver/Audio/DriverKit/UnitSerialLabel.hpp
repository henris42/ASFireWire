//
//  UnitSerialLabel.hpp
//  ASFWDriver
//
//  The unit-specific part of a device's identity, for the Core Audio device
//  name. Two units of the same model would otherwise be published under the
//  same name ("828mkII — ASFW" twice): Core Audio tells them apart by the
//  GUID-based device UID, but a person picking an output in the Sound pane or
//  a DAW cannot, and Audio MIDI Setup does not let the user rename a device.
//
//  IEEE 1212 config ROMs carry no standard serial-number entry and MOTU's ROM
//  has no vendor leaf for it either, so the serial comes from the EUI-64: the
//  low 40 bits are the vendor's unit extension. MOTU numbers its units there
//  (observed 0x0001f2000000f944 and 0x0001f2000001f717 for two 828mkII, i.e.
//  63812 and 128791), so for MOTU the extension is shown in decimal to match
//  the label on the unit. Other vendors get the extension in hex, which is at
//  least unique per unit.
//

#pragma once

#include "../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <cstdint>
#include <cstdio>

namespace ASFW::Isoch::Audio {

inline constexpr uint64_t kGuidUnitExtensionMask = 0xFF'FFFF'FFFFULL;

/// Writes the unit's serial label (without any "#" prefix) into `out`.
/// Returns the number of characters written, excluding the terminator, or 0
/// when nothing identifies the unit (GUID 0 or a buffer too small).
inline size_t FormatUnitSerial(uint32_t vendorId, uint64_t guid, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    const uint64_t extension = guid & kGuidUnitExtensionMask;
    if (guid == 0) {
        return 0;
    }
    const int written = (vendorId == ASFW::DeviceProfiles::Audio::kMotuVendorId)
        ? std::snprintf(out, capacity, "%llu", static_cast<unsigned long long>(extension))
        : std::snprintf(out, capacity, "%010llX", static_cast<unsigned long long>(extension));
    if (written <= 0 || static_cast<size_t>(written) >= capacity) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written);
}

} // namespace ASFW::Isoch::Audio
