//
// AdkPropertySerialization.cpp
// ASFWDriver
//
// See AdkPropertySerialization.hpp.
//

#include "AdkPropertySerialization.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSMetaClass.h>
#include <DriverKit/OSSerialization.h>

#include <atomic>

namespace ASFW::Audio::DriverKit {
namespace {

// Distinct values kept at most; past this a read is serialized as
// AudioDriverKit itself would, page and all. A device publishes a few dozen.
constexpr uint32_t kMaxCachedValues = 512;

// Created on first use. Plain pointers, constant-initialized: a static with a
// destructor would need __cxa_atexit, which DriverKit 21's libc++ lacks.
std::atomic<IOLock*> gCacheLock{nullptr};
OSDictionary* gCache = nullptr; // value -> serialization, under gCacheLock

[[nodiscard]] IOLock* CacheLock() noexcept {
    IOLock* lock = gCacheLock.load(std::memory_order_acquire);
    if (lock != nullptr) {
        return lock;
    }
    IOLock* const created = IOLockAlloc();
    if (created == nullptr) {
        return nullptr;
    }
    if (gCacheLock.compare_exchange_strong(lock, created, std::memory_order_acq_rel)) {
        return created;
    }
    IOLockFree(created);
    return lock;
}

// `value` serialized as IOUserAudioObject::_SerializeObject serializes it. The
// buffer finalizeBuffer() hands over stays allocated (see the header).
[[nodiscard]] OSSharedPtr<OSData> Serialize(OSString* value) noexcept {
    OSSerialization* const serialization = OSSerialization::createFromObject(value);
    if (serialization == nullptr) {
        return nullptr;
    }
    size_t length = 0;
    const void* const bytes = serialization->finalizeBuffer(&length);
    serialization->release();
    if (bytes == nullptr) {
        return nullptr;
    }
    return OSSharedPtr(OSData::withBytes(bytes, length), OSNoRetain);
}

// `value`'s serialization, made once per distinct value.
[[nodiscard]] OSSharedPtr<OSData> CachedSerialization(OSString* value) noexcept {
    IOLock* const lock = CacheLock();
    if (value == nullptr || lock == nullptr) {
        return nullptr;
    }
    IOLockLock(lock);
    if (gCache == nullptr) {
        gCache = OSDictionary::withCapacity(64);
    }
    OSSharedPtr<OSData> serialized;
    if (gCache != nullptr) {
        serialized = OSSharedPtr(OSDynamicCast(OSData, gCache->getObject(value)), OSRetain);
    }
    if (!serialized) {
        serialized = Serialize(value);
        if (serialized && gCache != nullptr && gCache->getCount() < kMaxCachedValues) {
            (void)gCache->setObject(value, serialized.get());
        }
    }
    IOLockUnlock(lock);
    return serialized;
}

} // namespace

OSSharedPtr<OSString> LeakProneObjectString(IOUserAudioObject& object,
                                            const AudioDriverKit::IOUserAudioObjectPropertyAddress* address) {
    if (!kAnswerSerializedProperties || address == nullptr) {
        return nullptr;
    }
    switch (address->mSelector) {
    case kAdkSelectorName:
        return object.GetName();
    case kAdkSelectorElementName:
        return object.GetElementName(address->mElement, address->mScope);
    case kAdkSelectorElementCategoryName:
        return object.GetElementCategoryName(address->mElement, address->mScope);
    case kAdkSelectorElementNumberName:
        return object.GetElementNumberName(address->mElement, address->mScope);
    default:
        return nullptr;
    }
}

kern_return_t AnswerPropertyData(OSString* value, OSData** outData) noexcept {
    if (outData == nullptr) {
        return kIOReturnBadArgument;
    }
    // A copy: the reply is the caller's, the cached serialization is shared.
    const auto serialized = CachedSerialization(value);
    *outData = serialized ? OSData::withBytes(serialized->getBytesNoCopy(), serialized->getLength()) : nullptr;
    return *outData != nullptr ? kIOReturnSuccess : kIOReturnNoMemory;
}

kern_return_t AnswerPropertySize(OSString* value, size_t* outSize) noexcept {
    if (outSize == nullptr) {
        return kIOReturnBadArgument;
    }
    const auto serialized = CachedSerialization(value);
    if (!serialized) {
        return kIOReturnNoMemory;
    }
    *outSize = serialized->getLength();
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio::DriverKit
