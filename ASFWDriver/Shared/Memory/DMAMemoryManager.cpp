#include "DMAMemoryManager.hpp"
#include "UncachedFill.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <cstdio>
#include <DriverKit/IOLib.h>

#include "../../Hardware/HardwareInterface.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Common/BarrierUtils.hpp"

namespace ASFW::Shared {

namespace {
constexpr size_t kTracePreviewBytes = 64;
// Smallest page size of any supported Mac; a region at most this large that
// is aligned to its own size is physically contiguous on 4 KiB and 16 KiB pages.
constexpr size_t kPageGranule = 4096;
std::atomic<bool> gDMACoherencyTraceEnabled{false};
} // namespace

void DMAMemoryManager::SetTracingEnabled(bool enabled) noexcept {
    const bool previous = gDMACoherencyTraceEnabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled) {
        return;
    }
    ASFW_LOG(Async, "DMAMemoryManager: coherency tracing %{public}s",
             enabled ? "ENABLED" : "disabled");
}

bool DMAMemoryManager::IsTracingEnabled() noexcept {
    return gDMACoherencyTraceEnabled.load(std::memory_order_acquire);
}

DMAMemoryManager::~DMAMemoryManager() { Reset(); }

void DMAMemoryManager::Reset() noexcept {
    // Release CPU mapping first
    if (dmaMemoryMap_) {
        dmaMemoryMap_->release();
        dmaMemoryMap_ = nullptr;
    }

    // Tear down the DMA mappings next
    for (auto& command : dmaCommands_) {
        if (command) {
            command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        }
    }
    dmaCommands_.clear();
    runs_.clear();
    runsByIOVA_.clear();

    // Release backing buffer last
    dmaBuffer_.reset();

    slabVirt_ = nullptr;
    slabIOVA_ = 0;
    slabSize_ = 0;
    mappingLength_ = 0;
    cursor_ = 0;
}

bool DMAMemoryManager::Initialize(Driver::HardwareInterface& hw, size_t totalSize) {
    ASFW_LOG(Async, "DMAMemoryManager: Initializing with totalSize=%zu", totalSize);

    if (slabVirt_ != nullptr) {
        ASFW_LOG(Async, "DMAMemoryManager: Already initialized");
        return false;
    }

    if (totalSize == 0) {
        ASFW_LOG_ERROR(Async, "DMAMemoryManager::Initialize: totalSize=0");
        return false;
    }

    // Whole pages: the mapping is page aligned and regions are placed so none
    // of them straddles a page it should not.
    const size_t alignedSize = (totalSize + (kPageGranule - 1)) & ~(kPageGranule - 1);

    ASFW_LOG(Async, "DMAMemoryManager: Allocating %zu bytes (requested %zu), scattered pages",
             alignedSize, totalSize);

    auto dmaBufferOpt = hw.AllocateDMAPaged(alignedSize, kIOMemoryDirectionInOut);
    if (!dmaBufferOpt.has_value()) {
        ASFW_LOG(Async, "DMAMemoryManager: AllocateDMAPaged failed for %zu bytes", alignedSize);
        return false;
    }

    auto& dmaBufferInfo = dmaBufferOpt.value();
    dmaBuffer_ = dmaBufferInfo.descriptor;
    dmaCommands_ = std::move(dmaBufferInfo.windowCommands);  // CRITICAL: keep the mappings alive
    mappingLength_ = dmaBufferInfo.length;

    runs_.clear();
    runs_.reserve(dmaBufferInfo.segments.size());
    size_t offset = 0;
    for (const auto& segment : dmaBufferInfo.segments) {
        if (segment.length == 0 || segment.address + segment.length - 1 > 0xFFFFFFFFULL ||
            (segment.address & 0xF) != 0) {
            ASFW_LOG_ERROR(Async, "DMAMemoryManager: unusable page run iova=0x%llx len=%llu",
                           segment.address, segment.length);
            Reset();
            return false;
        }
        runs_.push_back(PageRun{.offset = offset, .length = static_cast<size_t>(segment.length),
                                .iova = segment.address});
        offset += static_cast<size_t>(segment.length);
    }
    if (runs_.empty() || offset < alignedSize) {
        ASFW_LOG_ERROR(Async, "DMAMemoryManager: page runs cover %zu of %zu bytes", offset, alignedSize);
        Reset();
        return false;
    }
    runsByIOVA_.resize(runs_.size());
    for (size_t i = 0; i < runs_.size(); ++i) {
        runsByIOVA_[i] = i;
    }
    std::sort(runsByIOVA_.begin(), runsByIOVA_.end(),
              [this](size_t a, size_t b) { return runs_[a].iova < runs_[b].iova; });
    slabIOVA_ = runs_.front().iova;

    // Create uncached mapping (cache-inhibit mode)
    // macOS DriverKit reliably supports this mode - writes bypass CPU cache
    kern_return_t kr = dmaBuffer_->CreateMapping(
        /*options*/  kIOMemoryMapCacheModeInhibit,  // Uncached mapping
        /*address*/  0,
        /*offset*/   0,
        /*length*/   alignedSize,
        /*alignment*/0,
        &dmaMemoryMap_);

    if (kr != kIOReturnSuccess || dmaMemoryMap_ == nullptr) {
        ASFW_LOG_ERROR(Async, "DMAMemoryManager: CreateMapping failed, kr=0x%08x", kr);
        Reset();
        return false;
    }

    // CRITICAL: Use CPU mapping's actual length, not DMA segment length
    mappingLength_ = static_cast<size_t>(dmaMemoryMap_->GetLength());
    if (mappingLength_ < alignedSize) {
        ASFW_LOG_ERROR(Async,
            "DMAMemoryManager::Initialize: CPU map shorter than requested: mapLen=%zu < need=%zu",
            mappingLength_, alignedSize);
        Reset();
        return false;
    }

    slabVirt_ = reinterpret_cast<uint8_t*>(dmaMemoryMap_->GetAddress());
    if (slabVirt_ == nullptr) {
        ASFW_LOG(Async, "DMAMemoryManager: Mapping returned null virtual address");
        Reset();
        return false;
    }
    if ((reinterpret_cast<uintptr_t>(slabVirt_) & (kPageGranule - 1)) != 0) {
        // Region placement relies on page-aligned offsets being page aligned
        // in memory too; a mapping never starts mid-page, so this is a guard.
        ASFW_LOG_ERROR(Async, "DMAMemoryManager: mapping %p is not page aligned", slabVirt_);
        Reset();
        return false;
    }

    // Verify mapping is writable (sanity probe)
    volatile uint8_t* probe = slabVirt_;
    uint8_t tmp = *probe;  // Read must work
    *(const_cast<uint8_t*>(probe)) = tmp;  // Tiny write; if this faults, mapping is RO

    slabSize_ = alignedSize;
    cursor_ = 0;

    // Zero entire slab for deterministic descriptor state
    ZeroSlab(slabSize_);

    ASFW_LOG(Async,
             "DMAMemoryManager: Initialized - vaddr=%p first iova=0x%llx size=%zu mapped=%zu page runs=%zu",
             slabVirt_, slabIOVA_, slabSize_, mappingLength_, runs_.size());
    ASFW_LOG(Async,
             "  Cache mode: UNCACHED (kIOMemoryMapCacheModeInhibit)");
    ASFW_LOG(Async,
             "  Alignment: 16B (OHCI §1.7), CPU writes bypass cache → RAM directly");

    return true;
}

std::optional<DMAMemoryManager::Region> DMAMemoryManager::AllocateRegion(size_t size, size_t alignment) {
    if (slabVirt_ == nullptr) {
        ASFW_LOG(Async, "DMAMemoryManager: AllocateRegion called before Initialize");
        return std::nullopt;
    }

    if (size == 0) {
        ASFW_LOG(Async, "DMAMemoryManager: AllocateRegion with size=0");
        return std::nullopt;
    }

    // Ensure minimal alignment of 16 bytes per OHCI spec
    if (alignment < 16) {
        alignment = 16;
    }

    // Verify power of 2
    if ((alignment & (alignment - 1)) != 0) {
        ASFW_LOG(Async, "DMAMemoryManager: Invalid alignment %zu (must be power of 2), forcing 16", alignment);
        alignment = 16;
    }

    // Enforce 16-byte alignment for size
    const size_t alignedSize = AlignSize(size);

    // The slab's pages are scattered. A region that fits in a page is aligned to
    // its own size so it cannot straddle one and stays physically contiguous;
    // a larger region starts on a page and is contiguous per page only.
    const bool paged = alignedSize > kPageGranule;
    alignment = std::max(alignment, paged ? kPageGranule : std::bit_ceil(alignedSize));

    // Align cursor to requested alignment
    size_t alignedCursor = (cursor_ + (alignment - 1)) & ~(alignment - 1);

    if (alignedCursor + alignedSize > slabSize_) {
        ASFW_LOG_ERROR(Async,
            "DMAMemoryManager: AllocateRegion would overflow - need %zu (align pad %zu), have %zu (slab=%zu cursor=%zu)",
            alignedSize, alignedCursor - cursor_, slabSize_ - cursor_, slabSize_, cursor_);
        return std::nullopt;
    }

    Region region{};
    region.virtualBase = slabVirt_ + alignedCursor;
    region.deviceBase = VirtToIOVA(reinterpret_cast<const std::byte*>(region.virtualBase));
    region.size = alignedSize;

    cursor_ = alignedCursor + alignedSize;

    ASFW_LOG(Async, "DMAMemoryManager: Allocated region - vaddr=%p iova=0x%llx size=%zu (requested %zu, align %zu%{public}s)",
             region.virtualBase, region.deviceBase, region.size, size, alignment,
             paged ? ", paged: translate per element" : "");

    return region;
}

bool DMAMemoryManager::AlignCursorToIOVA(size_t alignment) noexcept {
    if (slabVirt_ == nullptr || slabIOVA_ == 0 || slabSize_ == 0) {
        ASFW_LOG(Async, "DMAMemoryManager: AlignCursorToIOVA called before Initialize");
        return false;
    }

    if (alignment < 16) {
        alignment = 16;
    }
    // Verify power of 2
    if ((alignment & (alignment - 1)) != 0) {
        ASFW_LOG(Async, "DMAMemoryManager: AlignCursorToIOVA invalid alignment=%zu", alignment);
        return false;
    }

    const size_t alignedCursor = (cursor_ + (alignment - 1)) & ~(alignment - 1);
    if (alignedCursor > slabSize_) {
        ASFW_LOG_ERROR(Async,
            "DMAMemoryManager: AlignCursorToIOVA would overflow: cursor=%zu aligned=%zu slab=%zu",
            cursor_, alignedCursor, slabSize_);
        return false;
    }

    cursor_ = alignedCursor;

    ASFW_LOG(Async,
             "DMAMemoryManager: AlignCursorToIOVA(%zu) -> cursor=%zu (iova now 0x%llx)",
             alignment, cursor_,
             VirtToIOVA(reinterpret_cast<const std::byte*>(slabVirt_ + cursor_)));
    return true;
}

std::optional<size_t> DMAMemoryManager::RunForOffset(size_t offset) const noexcept {
    if (offset >= slabSize_) {
        return std::nullopt;
    }
    // runs_ is in offset order: last run whose offset <= offset.
    auto it = std::upper_bound(runs_.begin(), runs_.end(), offset,
                               [](size_t value, const PageRun& run) { return value < run.offset; });
    if (it == runs_.begin()) {
        return std::nullopt;
    }
    --it;
    if (offset >= it->offset + it->length) {
        return std::nullopt;
    }
    return static_cast<size_t>(it - runs_.begin());
}

std::optional<size_t> DMAMemoryManager::RunForIOVA(uint64_t iova) const noexcept {
    if (iova == 0 || runsByIOVA_.empty()) {
        return std::nullopt;
    }
    auto it = std::upper_bound(runsByIOVA_.begin(), runsByIOVA_.end(), iova,
                               [this](uint64_t value, size_t index) { return value < runs_[index].iova; });
    if (it == runsByIOVA_.begin()) {
        return std::nullopt;
    }
    --it;
    const PageRun& run = runs_[*it];
    if (iova >= run.iova + run.length) {
        return std::nullopt;
    }
    return *it;
}

uint64_t DMAMemoryManager::VirtToIOVA(const std::byte* virt) const noexcept {
    if (!IsInSlabRange(virt)) {
        return 0;
    }

    const auto* bytePtr = reinterpret_cast<const uint8_t*>(virt);
    const size_t offset = static_cast<size_t>(bytePtr - slabVirt_);
    const auto run = RunForOffset(offset);
    if (!run) {
        return 0;
    }
    return runs_[*run].iova + static_cast<uint64_t>(offset - runs_[*run].offset);
}

std::byte* DMAMemoryManager::IOVAToVirt(uint64_t iova) const noexcept {
    if (slabVirt_ == nullptr) {
        return nullptr;
    }
    const auto run = RunForIOVA(iova);
    if (!run) {
        return nullptr;
    }
    const size_t offset = runs_[*run].offset + static_cast<size_t>(iova - runs_[*run].iova);
    if (offset >= slabSize_) {
        return nullptr;
    }
    return reinterpret_cast<std::byte*>(slabVirt_ + offset);
}

bool DMAMemoryManager::IsInSlabRange(const std::byte* ptr) const noexcept {
    if (slabVirt_ == nullptr || ptr == nullptr) {
        return false;
    }

    const auto* bytePtr = reinterpret_cast<const uint8_t*>(ptr);
    return (bytePtr >= slabVirt_) && (bytePtr < (slabVirt_ + slabSize_));
}

bool DMAMemoryManager::IsInSlabRange(uint64_t iova) const noexcept {
    return RunForIOVA(iova).has_value();
}

void DMAMemoryManager::ZeroSlab(size_t length) noexcept {
    if (slabVirt_ == nullptr || length == 0) {
        return;
    }

    const size_t cappedLength = std::min(length, slabSize_);

    // Cache-inhibited mappings reject dc zva (UncachedFill.hpp).
    Shared::FillUncachedDma(slabVirt_, 0, cappedLength);
}

void DMAMemoryManager::PublishRange(const std::byte* address, size_t length) const noexcept {
    if (address == nullptr || length == 0) {
        ::ASFW::Driver::IoBarrier();
        return;
    }

    if (!IsInSlabRange(address)) {
        if (IsTracingEnabled()) {
            ASFW_LOG(Async,
                     "⚠️  PublishRange ignored: address %p (len=%zu) outside DMA slab",
                     address, length);
        }
        ::ASFW::Driver::IoBarrier();
        return;
    }

    if (IsTracingEnabled()) {
        TraceHexPreview("PublishRange", address, length);
        ASFW_LOG(Async,
                 "🧭 PublishRange: virt=%p len=%zu (uncached - barrier only)",
                 address, length);
    }

    // Uncached mode: CPU writes bypass cache, just need ordering barrier
    ::ASFW::Driver::IoBarrier();
}

void DMAMemoryManager::FetchRange(const std::byte* address, size_t length) const noexcept {
    if (address == nullptr || length == 0) {
        ::ASFW::Driver::IoBarrier();
        return;
    }

    if (!IsInSlabRange(address)) {
        if (IsTracingEnabled()) {
            ASFW_LOG(Async,
                     "⚠️  FetchRange ignored: address %p (len=%zu) outside DMA slab",
                     address, length);
        }
        ::ASFW::Driver::IoBarrier();
        return;
    }

    // Uncached mode: CPU reads see latest writes, just need ordering barrier
    ::ASFW::Driver::IoBarrier();

    if (IsTracingEnabled()) {
        TraceHexPreview("FetchRange", address, length);
        ASFW_LOG(Async,
                 "🧭 FetchRange: virt=%p len=%zu (uncached - barrier only)",
                 address, length);
    }
}

void DMAMemoryManager::TraceHexPreview(const char* tag,
                                       const std::byte* address,
                                       size_t length) const noexcept {
    if (!IsTracingEnabled() || address == nullptr || length == 0) {
        return;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(address);
    const size_t preview = std::min(length, kTracePreviewBytes);
    char line[3 * 16 + 1];

    for (size_t offset = 0; offset < preview; offset += 16) {
        const size_t chunk = std::min(static_cast<size_t>(16), preview - offset);
        char* cursor = line;
        size_t remaining = sizeof(line);
        for (size_t i = 0; i < chunk && remaining > 3; ++i) {
            const int written = std::snprintf(cursor, remaining, "%02X ", bytes[offset + i]);
            if (written <= 0) {
                break;
            }
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
        *cursor = '\0';
        ASFW_LOG(Async,
                 "    %{public}s +0x%02zx: %{public}s",
                 tag,
                 offset,
                 line);
    }
}

void DMAMemoryManager::HexDump64(const std::byte* address, const char* tag) const noexcept {
    // Align to 64-byte cache line boundary
    const auto* d = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(address) & ~uintptr_t(63));
    
    ASFW_LOG(Async, "[%{public}s] 64B@%p:", tag, d);
    ASFW_LOG(Async, "  [00-1F] %08x %08x %08x %08x  %08x %08x %08x %08x",
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    ASFW_LOG(Async, "  [20-3F] %08x %08x %08x %08x  %08x %08x %08x %08x",
             d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
}

} // namespace ASFW::Shared
