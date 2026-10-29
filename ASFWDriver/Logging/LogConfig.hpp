//
// LogConfig.hpp
// ASFWDriver
//
// Start-time logging setup. The log level itself lives in LogLevels.hpp.
//

#ifndef ASFW_LOGGING_LOGCONFIG_HPP
#define ASFW_LOGGING_LOGCONFIG_HPP

class IOService;

namespace ASFW::Logging {

/// Applies the controller's asfw-log-level property (a log level spec, set
/// for example by OpenCore DeviceProperties) unless something has set the
/// level since the dext started, then logs "LogConfig initialized: level ...
/// (from ...)". Called at the top of ASFWDriver::Start, before the first
/// start step.
void InitializeLogConfig(IOService* provider);

} // namespace ASFW::Logging

#endif // ASFW_LOGGING_LOGCONFIG_HPP
