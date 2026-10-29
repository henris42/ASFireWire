//
// ASFWAudioStream.cpp
// ASFWDriver
//
// See ASFWAudioStream.iig.
//

#include "ASFWAudioStream.h"
#include "AdkPropertySerialization.hpp"

#include <DriverKit/OSMetaClass.h>

OSSharedPtr<ASFWAudioStream> ASFWAudioStream::Create(IOUserAudioDriver* in_driver,
                                                     IOUserAudioStreamDirection in_direction,
                                                     IOMemoryDescriptor* in_io_memory_descriptor)
{
    auto stream = OSSharedPtr(OSTypeAlloc(ASFWAudioStream), OSNoRetain);
    if (!stream || !stream->init(in_driver, in_direction, in_io_memory_descriptor)) {
        return nullptr;
    }
    return stream;
}

kern_return_t ASFWAudioStream::_GetPropertySize(const IOUserAudioObjectPropertyAddress* in_prop_addr,
                                                OSData* in_qualifier_data,
                                                size_t* out_size)
{
    if (const auto value = ASFW::Audio::DriverKit::LeakProneObjectString(*this, in_prop_addr)) {
        return ASFW::Audio::DriverKit::AnswerPropertySize(value.get(), out_size);
    }
    return super::_GetPropertySize(in_prop_addr, in_qualifier_data, out_size);
}

kern_return_t ASFWAudioStream::_GetPropertyData(const IOUserAudioObjectPropertyAddress* in_prop_addr,
                                                OSData* in_qualifier_data,
                                                OSData** out_data)
{
    if (const auto value = ASFW::Audio::DriverKit::LeakProneObjectString(*this, in_prop_addr)) {
        return ASFW::Audio::DriverKit::AnswerPropertyData(value.get(), out_data);
    }
    return super::_GetPropertyData(in_prop_addr, in_qualifier_data, out_data);
}
