//
// LogConfig.cpp
// ASFWDriver
//

#include "LogConfig.hpp"

#include <DriverKit/IOService.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>

#include "Logging.hpp"

namespace ASFW::Logging {

namespace {

constexpr const char* kBootLevelProperty = "asfw-log-level";

// OpenCore injects a string device property as data with a trailing NUL.
void ApplyBootLevel(IOService* provider) {
    OSDictionary* properties = nullptr;
    if (provider->CopyProperties(&properties) != kIOReturnSuccess || properties == nullptr) {
        return;
    }
    const char* text = nullptr;
    size_t length = 0;
    OSObject* value = properties->getObject(kBootLevelProperty);
    if (auto* data = OSDynamicCast(OSData, value); data != nullptr) {
        text = static_cast<const char*>(data->getBytesNoCopy());
        length = data->getLength();
    } else if (auto* string = OSDynamicCast(OSString, value); string != nullptr) {
        text = string->getCStringNoCopy();
        length = string->getLength();
    }
    if (text != nullptr) {
        if (const auto settings = ParseLogLevelSpec(text, length)) {
            ApplyLogLevel(*settings, LogLevelSource::BootProperty);
        } else {
            ASFW_LOG_WARNING(Controller,
                             "[Log] %{public}s is not a log level (off, error, warn, info, debug or "
                             "trace, then optional category=level); ignored",
                             kBootLevelProperty);
        }
    }
    properties->release();
}

} // namespace

void InitializeLogConfig(IOService* provider) {
    if (provider != nullptr && CurrentLogLevelSource() == LogLevelSource::Default) {
        ApplyBootLevel(provider);
    }
    char spec[kLogLevelSpecCapacity];
    (void)FormatLogLevelSpec(CurrentLogLevel(), spec, sizeof(spec));
    ASFW_LOG_MILESTONE(Controller, "[Log] LogConfig initialized: level %{public}s (from %{public}s)",
                       spec, LogLevelSourceName(CurrentLogLevelSource()));
}

} // namespace ASFW::Logging
