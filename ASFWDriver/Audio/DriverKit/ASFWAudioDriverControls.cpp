//
// ASFWAudioDriverControls.cpp
// ASFWDriver
//
// Protocol-backed control forwarding for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"

kern_return_t ASFWAudioDriver::ApplyProtocolBooleanControl(uint32_t classIdFourCC,
                                                           uint32_t element,
                                                           bool value)
{
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    return ivars->device.audioNub->SetProtocolBooleanControl(classIdFourCC, element, value);
}

kern_return_t ASFWAudioDriver::ReadProtocolBooleanControl(uint32_t classIdFourCC,
                                                          uint32_t element,
                                                          bool* outValue)
{
    if (!outValue) {
        return kIOReturnBadArgument;
    }
    *outValue = false;
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    return ivars->device.audioNub->GetProtocolBooleanControl(classIdFourCC, element, outValue);
}

kern_return_t ASFWAudioDriver::ApplyDeviceClockSource(uint32_t source, uint32_t* outConfirmed)
{
    if (!outConfirmed) {
        return kIOReturnBadArgument;
    }
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    const kern_return_t status = ivars->device.audioNub->SetDeviceClockSource(source, outConfirmed);
    if (status == kIOReturnSuccess) {
        ivars->device.clockSource = *outConfirmed;
    }
    return status;
}

namespace ASFW::Audio::DriverKit {

void RefreshClockSourceControl(ASFWAudioDriver_IVars& ivars) noexcept
{
    if (!ivars.clockSourceControl || !ivars.device.audioNub) {
        return;
    }
    uint32_t source = 0;
    if (ivars.device.audioNub->GetDeviceClockSource(&source) != kIOReturnSuccess ||
        source == ivars.device.clockSource) {
        return;
    }
    const IOUserAudioSelectorValue value = source;
    const kern_return_t status = ivars.clockSourceControl->SetCurrentSelectedValues(&value, 1);
    ASFW_LOG(Audio, "[ClockSource] guid=0x%016llx device now on %u (was %u) kr=0x%x",
             ivars.device.guid, source, ivars.device.clockSource, status);
    if (status == kIOReturnSuccess) {
        ivars.device.clockSource = source;
    }
}

} // namespace ASFW::Audio::DriverKit

kern_return_t ASFWAudioDriver::ApplyAvcFeatureControl(uint32_t token, bool muteControl,
                                                     int32_t value, int32_t* outConfirmedValue) {
    if (!ivars || !ivars->device.audioNub) return kIOReturnNotReady;
    return ivars->device.audioNub->SetAvcFeatureControl(token, muteControl, value, outConfirmedValue);
}
