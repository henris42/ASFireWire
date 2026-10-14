// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DriverKitCompat.hpp - Which DriverKit APIs the deployment target can rely on.
//
// DriverKit headers carry almost no availability markup: a dext built with a
// newer SDK for an older deployment target links symbols the older runtime
// lacks, and dyld refuses to load it ("Symbol missing"). Each gate names the
// first runtime known to export the API. tools/check-dext-symbols.sh checks a
// build against a target's own runtime.

#pragma once

#define ASFW_DRIVERKIT_MIN __ENVIRONMENT_DRIVERKIT_VERSION_MIN_REQUIRED__

// IOService::SetPowerOverride: xnu-11215.61.5, macOS 15.2.
#define ASFW_HAS_SET_POWER_OVERRIDE (ASFW_DRIVERKIT_MIN >= 240200)

// Absent from DriverKit 21 (macOS 12.7.6), present in 25; 22-24 unchecked, so
// assume 25. Apple's documentation lists 21.0 for all of them, which is wrong.
//   IOUserAudioStream::SetLatency, IOUserAudioObject::GetOwnerObjectID,
//   IOUserAudioDevice::SetWantsStreamFormatsRestored,
//   IOUserAudioClockDevice::SetWantsControlsRestored
#define ASFW_HAS_ADK_RESTORE_AND_LATENCY (ASFW_DRIVERKIT_MIN >= 250000)
//   IOUserSCSIParallelInterfaceController bundled-task dispatch
#define ASFW_HAS_SCSI_BUNDLED_TASKS (ASFW_DRIVERKIT_MIN >= 250000)
