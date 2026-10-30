#include "StatusPublisher.hpp"
#include "../Logging/Logging.hpp"

#include <algorithm>
#include <string_view>

#ifdef ASFW_HOST_TEST
#include <mach/mach_time.h>
#else
#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserClient.h>
#endif

#include "../Async/Interfaces/IAsyncSubsystemPort.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Controller/ControllerStateMachine.hpp"
#include "MetricsSink.hpp"

namespace ASFW::Driver {

kern_return_t StatusPublisher::Prepare() {
    if (statusBlock_ != nullptr) {
        return kIOReturnSuccess;
    }

    IOBufferMemoryDescriptor* rawBuffer = nullptr;
    auto kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, sizeof(SharedStatusBlock),
                                               64, &rawBuffer);
    if (kr != kIOReturnSuccess || rawBuffer == nullptr) {
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }

    rawBuffer->SetLength(sizeof(SharedStatusBlock));
    statusMemory_ = OSSharedPtr(rawBuffer, OSNoRetain);

    IOMemoryMap* rawMap = nullptr;
    kr = rawBuffer->CreateMapping(0, 0, 0, 0, 0, &rawMap);
    if (kr != kIOReturnSuccess || rawMap == nullptr) {
        statusMemory_.reset();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }

    statusMap_ = OSSharedPtr(rawMap, OSNoRetain);
    statusBlock_ = reinterpret_cast<SharedStatusBlock*>(rawMap->GetAddress());
    if (!statusBlock_) {
        statusMap_.reset();
        statusMemory_.reset();
        return kIOReturnNoMemory;
    }

    std::memset(statusBlock_, 0, sizeof(SharedStatusBlock));
    statusBlock_->version = SharedStatusBlock::kVersion;
    statusBlock_->length = sizeof(SharedStatusBlock);
    statusBlock_->sequence = 0;
    statusBlock_->reason = static_cast<uint32_t>(SharedStatusReason::Boot);
    statusBlock_->updateTimestamp = mach_absolute_time();
    return kIOReturnSuccess;
}

void StatusPublisher::Reset() {
    statusBlock_ = nullptr;
    statusMemory_.reset();
    statusMap_.reset();
    statusSequence_.store(0, std::memory_order_release);
    lastAsyncCompletionMach_.store(0, std::memory_order_release);
    asyncTimeoutCount_.store(0, std::memory_order_release);
    watchdogTickCount_.store(0, std::memory_order_release);
    watchdogLastTickUsec_.store(0, std::memory_order_release);
}

void StatusPublisher::Publish(ControllerCore* controller,
                              const ASFW::Async::IAsyncSubsystemPort* asyncSubsystem,
                              SharedStatusReason reason, uint32_t detailMask) {
    if (!statusBlock_) {
        return;
    }

    SharedStatusBlock snapshot{};
    snapshot.version = SharedStatusBlock::kVersion;
    snapshot.length = sizeof(SharedStatusBlock);
    snapshot.reason = static_cast<uint32_t>(reason);
    snapshot.detailMask = detailMask;
    snapshot.updateTimestamp = mach_absolute_time();

    if (controller) {
        const auto state = controller->StateMachine().CurrentState();
        snapshot.controllerState = static_cast<uint32_t>(state);
        const std::string_view stateName = ToString(state);
        const size_t nameLength = std::min(stateName.size(), sizeof(snapshot.controllerStateName) - 1);
        std::memcpy(snapshot.controllerStateName, stateName.data(), nameLength);

        const auto& busMetrics = controller->Metrics().BusReset();
        snapshot.busResetCount = busMetrics.resetCount;
        snapshot.lastBusResetStart = busMetrics.lastResetStart;
        snapshot.lastBusResetCompletion = busMetrics.lastResetCompletion;

        if (auto topo = controller->LatestTopology()) {
            snapshot.busGeneration = topo->generation;
            snapshot.nodeCount = topo->nodeCount;
            if (topo->localNodeId != Driver::kInvalidPhysicalId) {
                snapshot.localNodeID = static_cast<uint32_t>(topo->localNodeId);
            }
            if (topo->rootNodeId != Driver::kInvalidPhysicalId) {
                snapshot.rootNodeID = static_cast<uint32_t>(topo->rootNodeId);
            }
            if (topo->irmNodeId != Driver::kInvalidPhysicalId) {
                snapshot.irmNodeID = static_cast<uint32_t>(topo->irmNodeId);
            }

        }
    }

    if (asyncSubsystem) {
        const auto stats = asyncSubsystem->GetWatchdogStats();
        snapshot.watchdogTickCount = stats.tickCount;
        snapshot.watchdogLastTickUsec = stats.lastTickUsec;
        snapshot.asyncTimeouts = static_cast<uint32_t>(stats.expiredTransactions);
        snapshot.asyncPending = 0; // Placeholder until OutstandingTable exposes count
    }

    snapshot.asyncLastCompletion = lastAsyncCompletionMach_.load(std::memory_order_acquire);
    snapshot.asyncTimeouts = asyncTimeoutCount_.load(std::memory_order_acquire);
    snapshot.watchdogTickCount = watchdogTickCount_.load(std::memory_order_acquire);
    snapshot.watchdogLastTickUsec = watchdogLastTickUsec_.load(std::memory_order_acquire);

    if (snapshot.localNodeID != 0xFFFFFFFFu) {
        snapshot.flags |= SharedStatusBlock::kFlagLinkActive;
    }

    // Seqlock for the polling reader: odd while the block is written, the
    // next even value once it is whole. Writers come from the interrupt, the
    // watchdog and start/stop; one that finds another writing skips (the next
    // publish carries the same state).
    if (publishing_.exchange(true, std::memory_order_acquire)) {
        return;
    }
    const uint64_t sequence = statusSequence_.load(std::memory_order_relaxed) + 2;
    auto* sharedSequence = reinterpret_cast<std::atomic<uint64_t>*>(&statusBlock_->sequence);
    snapshot.sequence = sequence - 1;
    sharedSequence->store(sequence - 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(statusBlock_, &snapshot, sizeof(SharedStatusBlock));
    sharedSequence->store(sequence, std::memory_order_release);
    statusSequence_.store(sequence, std::memory_order_relaxed);
    publishing_.store(false, std::memory_order_release);
}

kern_return_t StatusPublisher::CopySharedMemory(uint64_t* options,
                                                IOMemoryDescriptor** memory) const {
    if (!memory) {
        return kIOReturnBadArgument;
    }
    if (!statusMemory_) {
        return kIOReturnNotReady;
    }

    auto descriptor = statusMemory_.get();
    descriptor->retain();
    *memory = descriptor;
    if (options) {
        *options = kIOUserClientMemoryReadOnly;
    }
    return kIOReturnSuccess;
}

void StatusPublisher::SetLastAsyncCompletion(uint64_t machTime) {
    lastAsyncCompletionMach_.store(machTime, std::memory_order_release);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void StatusPublisher::UpdateAsyncWatchdog(uint32_t asyncTimeoutCount, uint64_t watchdogTickCount,
                                          uint64_t watchdogLastTickUsec) {
    asyncTimeoutCount_.store(asyncTimeoutCount, std::memory_order_release);
    watchdogTickCount_.store(watchdogTickCount, std::memory_order_release);
    watchdogLastTickUsec_.store(watchdogLastTickUsec, std::memory_order_release);
}

} // namespace ASFW::Driver
