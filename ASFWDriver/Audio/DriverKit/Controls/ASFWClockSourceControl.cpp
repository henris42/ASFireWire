//
// ASFWClockSourceControl.cpp
// ASFWDriver
//
// The HAL clock source control. See ASFWClockSourceControl.iig.
//

#include "ASFWClockSourceControl.h"
#include "ASFWAudioDriver.h"
#include "../AdkPropertySerialization.hpp"
#include "../Config/AudioDriverConfig.hpp"
#include "../../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>

#include <cstring>

OSSharedPtr<ASFWClockSourceControl> ASFWClockSourceControl::Create(ASFWAudioDriver* ownerDriver)
{
    auto* control = OSTypeAlloc(ASFWClockSourceControl);
    if (!control) {
        return nullptr;
    }
    if (!control->init(ownerDriver)) {
        control->release();
        return nullptr;
    }
    return OSSharedPtr(control, OSNoRetain);
}

bool ASFWClockSourceControl::init(ASFWAudioDriver* ownerDriver)
{
    if (!ownerDriver) {
        return false;
    }
    // One device-wide source: main element, global scope.
    if (!super::init(ownerDriver, true, IOUserAudioObjectPropertyElementMain,
                     IOUserAudioObjectPropertyScope::Global, IOUserAudioClassID::ClockSourceControl)) {
        return false;
    }
    ivars = IONewZero(ASFWClockSourceControl_IVars, 1);
    if (!ivars) {
        return false;
    }
    ivars->ownerDriver = ownerDriver;
    return true;
}

void ASFWClockSourceControl::free()
{
    if (ivars) {
        IOSafeDeleteNULL(ivars, ASFWClockSourceControl_IVars, 1);
    }
    super::free();
}

kern_return_t ASFWClockSourceControl::HandleChangeSelectedValues(
    const IOUserAudioSelectorValue* in_control_values, size_t in_num_values)
{
    if (!ivars || !ivars->ownerDriver) {
        return kIOReturnNotReady;
    }
    if (in_control_values == nullptr || in_num_values != 1) {
        return kIOReturnBadArgument;
    }
    uint32_t confirmed = 0;
    const kern_return_t status = ivars->ownerDriver->ApplyDeviceClockSource(in_control_values[0], &confirmed);
    if (status != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio, "[ClockSource] HAL set %u failed kr=0x%x (%{public}s)", in_control_values[0], status,
                       ASFW::Logging::IOReturnName(status));
        return status;
    }
    // The device's answer, which is what the selector must show.
    const IOUserAudioSelectorValue value = confirmed;
    return SetCurrentSelectedValues(&value, 1);
}

namespace {

// The string AudioDriverKit would serialize for `address` when this build
// answers the read itself: the control's name, or the name of the source the
// 4-byte qualifier selects (AdkPropertySerialization.hpp). Null leaves the read
// to super.
OSSharedPtr<OSString> LeakProneControlString(ASFWClockSourceControl& control,
                                             const IOUserAudioObjectPropertyAddress* address,
                                             OSData* qualifier)
{
    namespace Adk = ASFW::Audio::DriverKit;
    if (auto value = Adk::LeakProneObjectString(control, address)) {
        return value;
    }
    if (!Adk::kAnswerSerializedProperties || address == nullptr ||
        address->mSelector != Adk::kAdkSelectorSelectorItemName || qualifier == nullptr ||
        qualifier->getLength() != sizeof(IOUserAudioSelectorValue)) {
        return nullptr;
    }
    IOUserAudioSelectorValue source = 0;
    std::memcpy(&source, qualifier->getBytesNoCopy(), sizeof(source));
    // Graph construction adds at most kMaxClockSources sources.
    IOUserAudioSelectorValueDescription sources[ASFW::Isoch::Audio::kMaxClockSources]{};
    const size_t count = control.GetControlValueDescriptions(sources, ASFW::Isoch::Audio::kMaxClockSources);
    for (size_t i = 0; i < count; ++i) {
        if (sources[i].m_value == source) {
            return sources[i].m_name;
        }
    }
    return nullptr;
}

} // namespace

kern_return_t ASFWClockSourceControl::_GetPropertySize(const IOUserAudioObjectPropertyAddress* in_prop_addr,
                                                       OSData* in_qualifier_data,
                                                       size_t* out_size)
{
    if (const auto value = LeakProneControlString(*this, in_prop_addr, in_qualifier_data)) {
        return ASFW::Audio::DriverKit::AnswerPropertySize(value.get(), out_size);
    }
    return super::_GetPropertySize(in_prop_addr, in_qualifier_data, out_size);
}

kern_return_t ASFWClockSourceControl::_GetPropertyData(const IOUserAudioObjectPropertyAddress* in_prop_addr,
                                                       OSData* in_qualifier_data,
                                                       OSData** out_data)
{
    if (const auto value = LeakProneControlString(*this, in_prop_addr, in_qualifier_data)) {
        return ASFW::Audio::DriverKit::AnswerPropertyData(value.get(), out_data);
    }
    return super::_GetPropertyData(in_prop_addr, in_qualifier_data, out_data);
}
