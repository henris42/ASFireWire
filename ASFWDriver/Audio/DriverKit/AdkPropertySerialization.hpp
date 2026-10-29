//
// AdkPropertySerialization.hpp
// ASFWDriver
//
// AudioDriverKit answers a property whose value is an object -- an object or
// element name, a device or model UID, the manufacturer, a selector item name
// -- by serializing that object with OSSerialization. finalizeBuffer() hands
// the buffer to its caller, and in DriverKit 21 (macOS 12)
// IOUserAudioObject::_SerializeObject and _GetSerializedObjectLength never free
// it: every such read, size or data, leaves a page of anonymous memory in the
// dext (b158: a 124 KB region per device start, as CoreAudio and its clients
// read the names and UIDs again around every IO start). DriverKit 25 frees the
// buffer with OSSerialization::freeBuffer.
//
// A dext cannot free that buffer on DriverKit 21 either: vm_deallocate is not
// exported to dexts, and freeBuffer does not exist there. So while the runtime
// leaks (ASFW_ADK_LEAKS_SERIALIZED_PROPERTIES), the device, stream and control
// subclasses answer those reads themselves from a cache: each distinct value
// is serialized once, by the same serializer, and costs its page once instead
// of on every read.

#pragma once

#include "../../Common/DriverKitCompat.hpp"

#include <AudioDriverKit/IOUserAudioObject.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSString.h>

#include <cstddef>
#include <cstdint>

namespace ASFW::Audio::DriverKit {

// Whether this build answers the leaking reads itself (DriverKitCompat.hpp).
inline constexpr bool kAnswerSerializedProperties = ASFW_ADK_LEAKS_SERIALIZED_PROPERTIES;

// The selectors AudioDriverKit answers by serializing a string.
inline constexpr uint32_t kAdkSelectorName = static_cast<uint32_t>('lnam');
inline constexpr uint32_t kAdkSelectorElementName = static_cast<uint32_t>('lchn');
inline constexpr uint32_t kAdkSelectorElementCategoryName = static_cast<uint32_t>('lccn');
inline constexpr uint32_t kAdkSelectorElementNumberName = static_cast<uint32_t>('lcnn');
inline constexpr uint32_t kAdkSelectorDeviceUID = static_cast<uint32_t>('uid ');
inline constexpr uint32_t kAdkSelectorModelUID = static_cast<uint32_t>('muid');
inline constexpr uint32_t kAdkSelectorManufacturer = static_cast<uint32_t>('lmak');
inline constexpr uint32_t kAdkSelectorSelectorItemName = static_cast<uint32_t>('scin');

// The string AudioDriverKit would serialize for `address` among the properties
// every IOUserAudioObject has -- its name and its element names -- when this
// build answers that read itself. Null leaves the read to AudioDriverKit:
// another selector, a property without a value (nothing is serialized then),
// or a runtime that does not leak.
[[nodiscard]] OSSharedPtr<OSString> LeakProneObjectString(
    IOUserAudioObject& object, const AudioDriverKit::IOUserAudioObjectPropertyAddress* address);

// A property read answered with `value` serialized as AudioDriverKit
// serializes it, from the cache.
[[nodiscard]] kern_return_t AnswerPropertyData(OSString* value, OSData** outData) noexcept;
[[nodiscard]] kern_return_t AnswerPropertySize(OSString* value, size_t* outSize) noexcept;

} // namespace ASFW::Audio::DriverKit
