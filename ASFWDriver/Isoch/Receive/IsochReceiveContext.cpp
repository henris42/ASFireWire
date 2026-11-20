#include "IsochReceiveContext.hpp"
#include "../Core/IsochEventGroup.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Diagnostics/Signposts.hpp"

#include <utility>

namespace ASFW::Isoch {

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<IsochReceiveContext> IsochReceiveContext::Create(::ASFW::Driver::HardwareInterface* hw,
                                                            std::shared_ptr<::ASFW::Isoch::Memory::IIsochDMAMemory> dmaMemory) {
    auto ctx = std::unique_ptr<IsochReceiveContext>(new (std::nothrow) IsochReceiveContext());
    if (!ctx) return nullptr;

    ctx->hardware_ = hw;
    ctx->dmaMemory_ = std::move(dmaMemory);

    return ctx;
}

// ============================================================================
// Lifecycle
// ============================================================================

IsochReceiveContext::~IsochReceiveContext() {
    (void)Stop();
}

// ============================================================================
// Configuration
// ============================================================================

IsochReceiveContext::Registers IsochReceiveContext::GetRegisters(uint8_t index) const {
    return Registers{
        .CommandPtr          = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvCommandPtr(index)),
        .ContextControlSet   = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvContextControlSet(index)),
        .ContextControlClear = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvContextControlClear(index)),
        .ContextMatch        = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvContextMatch(index)),
    };
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t IsochReceiveContext::Configure(uint8_t channel, uint8_t contextIndex) {
    if (!hardware_ || !dmaMemory_) {
        return kIOReturnNotReady;
    }

    // OHCI implements up to 32 IR contexts; the lanes use at most eight.
    if (contextIndex >= 8) {
        return kIOReturnBadArgument;
    }

    contextIndex_ = contextIndex;
    channel_ = channel;
    registers_ = GetRegisters(contextIndex_);
    return rxRing_.SetupRings(*dmaMemory_, kNumDescriptors, kMaxPacketSize);
}

// ============================================================================
// Runtime
// ============================================================================

kern_return_t IsochReceiveContext::Start() {
    unsigned waits = 0;
    while (rxLock_.test_and_set(std::memory_order_acquire)) {
        if (waits++ == 100) {
            ASFW_LOG_ERROR(Isoch, "[StopTrace] stage=rx-start-gate context=%u timeout", contextIndex_);
            return kIOReturnTimeout;
        }
        IOSleep(1);
    }
    struct GateScope final {
        std::atomic_flag& gate;
        ~GateScope() { gate.clear(std::memory_order_release); }
    } gateScope{rxLock_};
    if (GetState() != IRPolicy::State::Stopped) {
        return kIOReturnInvalid;
    }

    if (!hardware_) {
        ASFW_LOG_ERROR(Isoch, "❌ Start: hardware_ is null!");
        return kIOReturnNotReady;
    }

    const uint32_t cmdPtr = rxRing_.InitialCommandPtrWord();
    if (cmdPtr == 0) {
        ASFW_LOG_ERROR(Isoch, "❌ Start: Invalid descriptor cmdPtr");
        return kIOReturnInternalError;
    }
    const uint32_t contextMatch = 0xF0000000 | (channel_ & 0x3F);
    const uint32_t ctlValue = Driver::ContextControl::kRun | Driver::ContextControl::kIsochHeader;
    const uint32_t contextMask = 1u << contextIndex_;
    uint32_t ctrlBefore = 0;
    uint32_t pendingEvents = 0;
    {
        auto access = hardware_->TryBeginAccess();
        if (!access) return kIOReturnNotReady;
        // What the context held before this start (one line per start). The
        // Set address returns the unmasked events.
        ctrlBefore = access.Read(registers_.ContextControlSet);
        pendingEvents = access.Read(ASFW::Driver::Register32::kIsoRecvIntEventSet);
        access.Write(registers_.ContextMatch, contextMatch);
        access.Write(registers_.CommandPtr, cmdPtr);
        access.Write(registers_.ContextControlClear, 0xFFFFFFFFu);
        // Stopping a context sets its event bit (OHCI 1.2 draft §3.1.1.3).
        // Clear this context's stale bit and unmask it before RUN, as Linux
        // ohci_start_iso does (ohci.c:3209), so the clear cannot take a
        // completion of this run.
        access.Write(ASFW::Driver::Register32::kIsoRecvIntEventClear, contextMask);
        access.Write(ASFW::Driver::Register32::kIsoRecvIntMaskSet, contextMask);
        access.Write(registers_.ContextControlSet, ctlValue);
    }
    ASFW_LOG_DEBUG(Isoch, "IR: start ctx=%u ctrlBefore=0x%08x pendingEvents=0x%08x",
                   contextIndex_, ctrlBefore, pendingEvents);

    IOLockLock(lock_);
    Transition(IRPolicy::State::Running, 0, "Start");
    IOLockUnlock(lock_);
    rxRing_.ResetForStart();
    linkCounters_.Reset();
    pollRunning_.store(true, std::memory_order_release);

    if (receiveConsumer_) {
        receiveConsumer_->SetReceivePollSource(this);
        receiveConsumer_->OnReceiveActivated();
    }
    return kIOReturnSuccess;
}

kern_return_t IsochReceiveContext::Stop() {
    unsigned waits = 0;
    while (rxLock_.test_and_set(std::memory_order_acquire)) {
        if (waits++ == 100) {
            ASFW_LOG_ERROR(Isoch,
                "[StopTrace] stage=rx-poll-gate context=%u timeout action=retain-binding", contextIndex_);
            return kIOReturnTimeout;
        }
        IOSleep(1);
    }

    if (GetState() == IRPolicy::State::Stopped) {
        rxLock_.clear(std::memory_order_release);
        return kIOReturnSuccess;
    }

    const uint32_t contextMask = 1u << contextIndex_;
    if (auto access = hardware_->TryBeginAccess()) {
        access.Write(ASFW::Driver::Register32::kIsoRecvIntMaskClear, contextMask);
        access.WriteAndFlush(registers_.ContextControlClear, Driver::ContextControl::kRun);
    } else {
        if (hardware_->HardwareGone()) {
            // Provider revocation is proof that this controller cannot issue a
            // late DMA write. Retaining the direct binding in that case turns
            // a safe surprise-removal into a leak/UAF hazard.
            IOLockLock(lock_);
            Transition(IRPolicy::State::Stopped, 0, "Stop/provider-gone");
            IOLockUnlock(lock_);
            pollRunning_.store(false, std::memory_order_release);
            if (receiveConsumer_) {
                receiveConsumer_->OnReceiveQuiesced();
                receiveConsumer_->SetReceivePollSource(nullptr);
            }
            ASFW_LOG(Isoch,
                     "[Lifecycle] IR stop context=%u hardware-gone action=release-direct-binding",
                     contextIndex_);
            rxLock_.clear(std::memory_order_release);
            return kIOReturnSuccess;
        }
        // The context stays bound, but no thread outside the poll path may
        // keep reaching it: a later teardown would leave it dangling.
        if (receiveConsumer_) {
            receiveConsumer_->SetReceivePollSource(nullptr);
        }
        rxLock_.clear(std::memory_order_release);
        return kIOReturnNotReady;
    }
    // Flush RUN-clear and wait for ACTIVE to fall before dropping the direct
    // audio binding.  See Linux firewire/ohci.c:1361-1378 for the same
    // teardown ordering; freeing this mapping while ACTIVE is set can fault
    // the host when OHCI completes a late DMA write.
    ASFW_LOG_DEBUG(Isoch, "Stop: Disabled IR interrupt for context %u", contextIndex_);

    // Two complementary guards, both required: the revocable access scope keeps
    // us from issuing MMIO after Detach, and the all-ones sentinel catches a
    // physically removed device, which still reads 0xFFFFFFFF through a live scope.
    const auto readControl = [this] {
        auto access = hardware_->TryBeginAccess();
        return access ? access.Read(registers_.ContextControlSet) : 0U;
    };
    const uint32_t initialControl = readControl();
    if (initialControl != 0xFFFFFFFFu &&
        (initialControl & Driver::ContextControl::kActive) != 0) {
        IODelay(5);
        constexpr uint32_t kMaxIterations = 250;
        constexpr uint32_t kBaseDelayMicros = 6;
        for (uint32_t iteration = 0; iteration < kMaxIterations; ++iteration) {
            const uint32_t polledControl = readControl();
            if (polledControl == 0xFFFFFFFFu ||
                (polledControl & Driver::ContextControl::kActive) == 0) {
                break;
            }
            IODelay(kBaseDelayMicros + iteration);
        }
    }

    const uint32_t control = readControl();
    if (control != 0xFFFFFFFFu &&
        (control & Driver::ContextControl::kActive) != 0) {
        const kern_return_t failure = (control & Driver::ContextControl::kDead) != 0
            ? kIOReturnDMAError
            : kIOReturnTimeout;
        ASFW_LOG_ERROR(Isoch,
                       "IR: stop did not quiesce context=%u control=0x%08x kr=0x%08x (%{public}s); retaining direct binding",
                       contextIndex_, control, failure, ASFW::Logging::IOReturnName(failure));
        if (receiveConsumer_) {
            receiveConsumer_->SetReceivePollSource(nullptr);
        }
        rxLock_.clear(std::memory_order_release);
        return failure;
    }

    IOLockLock(lock_);
    Transition(IRPolicy::State::Stopped, 0, "Stop");
    IOLockUnlock(lock_);
    pollRunning_.store(false, std::memory_order_release);

    if (receiveConsumer_) {
        receiveConsumer_->OnReceiveQuiesced();
        receiveConsumer_->SetReceivePollSource(nullptr);
    }

    rxLock_.clear(std::memory_order_release);
    return kIOReturnSuccess;
}

uint32_t IsochReceiveContext::Poll() {
    uint32_t processed = 0;
    (void)PollOnce(processed, nullptr);
    return processed;
}

bool IsochReceiveContext::TryPoll(uint32_t& processed,
                                  const IsochReceiveDrainLimit& limit) noexcept {
    return PollOnce(processed, &limit);
}

// readerLimit: a reader's pull (TryPoll), which stops once the reader has
// what it needs or its deadline passed; nullptr for the interrupt and the
// watchdog, which drain everything completed.
bool IsochReceiveContext::PollOnce(uint32_t& processed,
                                   const IsochReceiveDrainLimit* readerLimit) noexcept {
    const bool readerPull = readerLimit != nullptr;
    processed = 0;
    if (rxLock_.test_and_set(std::memory_order_acquire)) {
        return false;
    }

    if (!pollRunning_.load(std::memory_order_acquire)) {
        rxLock_.clear(std::memory_order_release);
        return true;
    }

    const auto cycleHostPair =
        hardware_
            ? hardware_->ReadCycleTimeAndUpTime()
            : std::pair<uint32_t, uint64_t>{0, mach_absolute_time()};
    const uint32_t drainCycleTimer = cycleHostPair.first;
    const uint64_t drainHostTicks = cycleHostPair.second;
    const IsochReceiveBatch receiveBatch{
        .drainCycleTimer = drainCycleTimer,
        .drainHostTicks = drainHostTicks,
        .readerPull = readerPull,
        .link = &linkCounters_,
    };
    if (receiveConsumer_) {
        receiveConsumer_->BeginReceiveBatch(receiveBatch);
    }

    const auto onPacket = [this, drainHostTicks, drainCycleTimer, receiveBatch](
            const Rx::IsochRxDmaRing::CompletedPacket& pkt) {
        uint64_t callbackTimestamp = 0;
        linkCounters_.CountPacket(pkt.xferStatus, pkt.payload, pkt.actualLength);
        if (receiveConsumer_) {
            receiveConsumer_->ConsumePacket(
                receiveBatch,
                IsochReceivePacket{
                    .descriptorIndex = pkt.descriptorIndex,
                    .transferStatus = pkt.xferStatus,
                    .residualCount = pkt.resCount,
                    .payload = pkt.payload
                        ? std::span<const uint8_t>(pkt.payload, pkt.actualLength)
                        : std::span<const uint8_t>{},
                });
        }
        if (callback_) {
            const auto span = std::span<const uint8_t>(pkt.payload, pkt.actualLength);
            callback_(span,
                      static_cast<uint32_t>(pkt.xferStatus),
                      callbackTimestamp);
        }
    };
    processed = readerLimit
        ? rxRing_.DrainCompleted(*dmaMemory_, onPacket,
                                 [readerLimit] { return readerLimit->Reached(mach_absolute_time()); })
        : rxRing_.DrainCompleted(*dmaMemory_, onPacket);

    if (receiveConsumer_) {
        receiveConsumer_->EndReceiveBatch(receiveBatch);
    }

    rxLock_.clear(std::memory_order_release);
    return true;
}

void IsochReceiveContext::SetCallback(IsochReceiveCallback callback) {
    callback_ = callback;
}

void IsochReceiveContext::SetReceiveConsumer(
    IIsochReceiveConsumer* consumer) noexcept {
    receiveConsumer_ = consumer;
}

// The consumer's telemetry and diagnostics touch state its packet path
// writes, and a poll can run on another thread (TryPoll: the BeginRead pull).
// Take the poll gate; a poll in progress means this tick is skipped, and the
// next one (the watchdog's, about every 100 ms) catches up.
void IsochReceiveContext::DrainZtsTelemetry(uint32_t maxRecords) {
    if (!receiveConsumer_ || rxLock_.test_and_set(std::memory_order_acquire)) {
        return;
    }
    receiveConsumer_->DrainReceiveTelemetry(maxRecords);
    rxLock_.clear(std::memory_order_release);
}

void IsochReceiveContext::ServiceConsumerDiagnostics() {
    if (!receiveConsumer_ || rxLock_.test_and_set(std::memory_order_acquire)) {
        return;
    }
    receiveConsumer_->ServiceConsumerDiagnostics();
    rxLock_.clear(std::memory_order_release);
}

} // namespace ASFW::Isoch
