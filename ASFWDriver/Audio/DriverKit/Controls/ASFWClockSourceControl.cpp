//
// ASFWClockSourceControl.cpp
// ASFWDriver
//
// The HAL clock source control. See ASFWClockSourceControl.iig.
//

#include "ASFWClockSourceControl.h"
#include "ASFWAudioDriver.h"
#include "../../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>

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
        ASFW_LOG(Audio, "[ClockSource] HAL set %u failed kr=0x%x (%{public}s)", in_control_values[0], status,
                 ASFW::Logging::IOReturnName(status));
        return status;
    }
    // The device's answer, which is what the selector must show.
    const IOUserAudioSelectorValue value = confirmed;
    return SetCurrentSelectedValues(&value, 1);
}
