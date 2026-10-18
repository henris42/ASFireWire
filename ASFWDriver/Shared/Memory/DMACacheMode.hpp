#pragma once

#include <cstdint>

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOMemoryDescriptor.h>
#endif

namespace ASFW::Shared {

// CPU mapping cache mode for DMA slabs and buffers.
//
// Intel: DMA snoops the caches, so an ordinary cached mapping plus the
// barriers already in place (OSSynchronizeIO is mfence) is what Apple's own
// kernel drivers use. A cache-inhibited user mapping of a large, scattered,
// kernel-shared buffer never returned from the kernel on macOS 12 (b52: the
// dext's Start() sat in CreateMapping forever and held the boot).
//
// Apple silicon: keep the cache-inhibited mapping the driver was written with.
#if defined(__x86_64__)
inline constexpr uint64_t kDMAMappingCacheMode = kIOMemoryMapCacheModeDefault;
inline constexpr bool kDMAMappingIsUncached = false;
#else
inline constexpr uint64_t kDMAMappingCacheMode = kIOMemoryMapCacheModeInhibit;
inline constexpr bool kDMAMappingIsUncached = true;
#endif

} // namespace ASFW::Shared
