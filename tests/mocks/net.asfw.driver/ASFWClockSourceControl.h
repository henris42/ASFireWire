// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Host stand-in for the IIG-generated ASFWClockSourceControl.h. The host tests
// only hold one in the audio driver's ivars, so an owned, refcounted object is
// all the private header needs.

#pragma once

#include <DriverKit/OSObject.h>

class ASFWClockSourceControl : public OSObject {};
