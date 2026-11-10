// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IoRestartReasons.hpp -- reasons the transport side hands the audio driver
// through ASFWAudioNub::NotifyIoRestartRequired beyond DuplexRestartReason.

#pragma once

#include <cstdint>

namespace ASFW::Audio::Runtime {

/// The endpoint's stream layout changed (a MOTU optical mode) and the new
/// description is staged on its AudioEndpointRuntime. The audio driver
/// installs it inside an ADK configuration window instead of restarting IO:
/// the HAL stops every client, the stream formats and channel names change,
/// and the HAL restarts the clients on the same Core Audio device. Outside
/// DuplexRestartReason's range ('GEOM').
inline constexpr uint32_t kIoRestartReasonGeometryChange = 0x47454F4DU;

} // namespace ASFW::Audio::Runtime
