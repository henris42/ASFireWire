#pragma once
#include <cstring>

#include "ATManager.hpp"
#include "ATTrace.hpp"
#include "../Contexts/ContextBase.hpp"  // For ATRequestTag, ATResponseTag full definitions
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../Core/LockPolicy.hpp"  // Phase 1.2: Fine-grained locking
#include "../../Logging/Logging.hpp"

using ASFW::Driver::kContextControlRunBit;
using ASFW::Driver::kContextControlWakeBit;
using ASFW::Driver::kContextControlDeadBit;
using ASFW::Driver::kContextControlActiveBit;

namespace ASFW::Async::Engine {

namespace {
inline const char* ToStringImpl(ATState s) noexcept {
    switch (s) {
        case ATState::IDLE: return "IDLE";
        case ATState::ARMING: return "ARMING";
        case ATState::RUNNING: return "RUNNING";
        case ATState::STOPPING: return "STOPPING";
        case ATState::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
} // anonymous namespace

inline const char* ToString(ATState s) noexcept {
    return ToStringImpl(s);
}

template<typename ContextT, typename RingT, typename RoleTag>
kern_return_t ATManager<ContextT, RingT, RoleTag>::Submit(DescriptorChain&& chain, const AsyncCmdOptions& opts) {
    if (chain.Empty()) {
        ASFW_LOG_ERROR(Async, "[%{public}s] Submit: Empty chain", RoleTag::kContextName);
        return kIOReturnBadArgument;
    }

    const uint32_t txid = chain.txid;

    // PATH decision using software state only (Apple's pattern)
    // From decompilation @ 0xDBBE line 109: if (*((_BYTE *)this + 28))
    // Apple checks ONLY software flag, never reads hardware registers.
    //
    // The decision and the hot-append are one critical section, under the
    // lock the context's completion scan takes too (ShareRingLock): the
    // previous packet can neither complete and be cleared nor let the scan
    // stop the drained context between the check, the link and the WAKE.
    bool triedP2 = false;
    {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);

        // Simple check: Is context marked as running in software?
        // Additional safety: Ring must still have descriptors we can link to
        const bool hasPrevLast = ring().PrevLastBlocks() > 0;
        const bool ringHasData = !ring().IsEmpty();
        if ((this->state_ == State::RUNNING) && hasPrevLast && ringHasData) {
            // PATH 2: Hot-append to running context (fire-and-forget)
            triedP2 = true;
            if (SubmitPath2Locked_(chain, txid) == kIOReturnSuccess) {
                return kIOReturnSuccess;
            }
        }
    }  // Lock automatically released here (RAII)

    if (triedP2) {
        // Fall through to PATH 1 fallback on failure
        ASFW_LOG_V2(Async, "[%{public}s] PATH 2 failed, falling back to PATH 1", RoleTag::kContextName);
    }

    // V1: Compact AT transmit one-liner for packet flow visibility
    const uint8_t totalBlocks = chain.TotalBlocks();
    ASFW_LOG_V2(Async, "📤 AT/TX: txid=%u blocks=%u (%{public}s)",
               txid, totalBlocks, triedP2 ? "PATH2" : "PATH1");

    // PATH 1: First submission or re-arm
    return SubmitPath1_(chain, txid, opts);
}

template<typename ContextT, typename RingT, typename RoleTag>
kern_return_t ATManager<ContextT, RingT, RoleTag>::SubmitPath1_(const DescriptorChain& chain, uint32_t txid, const AsyncCmdOptions& opts) {
    (void)opts;
    // Lock held for FSM transitions, and for programming the context with the
    // ring update, NOT while waiting for a previous packet to leave.

    // FSM transition under lock
    {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);
        Base::Transition(State::ARMING, txid, "path1_start");
    }

    // Z nibble must be total blocks per OHCI; use TotalBlocks() not firstBlocks
    const uint8_t z = ATSubmitPolicy::ComputeZ(chain.TotalBlocks());
    PublishChain_(chain);
    this->IoWriteFence();

    // If hardware still considers the context running (PATH-2 fallback case),
    // clear RUN before programming CommandPtr so the next RUN=1 transition is visible.
    // Also poll when RUN=0 but still ACTIVE — the state a bounded AT-quiesce
    // timeout in the bus-reset FSM leaves behind.
    if (ctx().IsRunning() || ctx().IsActive()) {
        clearRunAndPoll_();
        if (ctx().IsActive()) {
            // Programming CommandPtr on an ACTIVE context is illegal (OHCI
            // §3.1.1): the in-flight packet is lost and the FW643 was observed
            // (2026-09-24, LS-9000) to stay ACTIVE=1 forever afterwards, across
            // bus resets. Refuse instead; the requester's split-timeout retry
            // covers a dropped response, the caller's retry a dropped request.
            const uint32_t ctrl = ctx().ReadControl();
            const uint32_t cmdPtrReg = ctx().ReadCommandPtr();
            const uint64_t sinceDrainNs = NowNs() - ctx().LastDrainStopNs();
            ASFW_LOG_WARNING(Async, "ctx=%{public}s txid=%u gen=%u P1_ARM while ACTIVE after clearRun poll gave up ctrl=0x%08x cmdPtr=0x%08x state=%{public}s head=%zu tail=%zu prevLast=%u drainStops=%u sinceDrainUs=%llu",
                             RoleTag::kContextName, txid, generation_, ctrl, cmdPtrReg, ToString(this->state_),
                             ring().Head(), ring().Tail(), ring().PrevLastBlocks(),
                             ctx().DrainStopCount(), sinceDrainNs / 1000);
            if (!wedgeDumped_) {
                wedgeDumped_ = true;
                logWedgeSnapshot_(txid, cmdPtrReg);
            }
            IOLockWrapper lockWrapper(lock());
            ScopedLock guard(lockWrapper);
            Base::Transition(State::IDLE, txid, "arm_refused_active");
            return kIOReturnBusy;
        }
    }

    const uint32_t cmdPtr = ring().CommandPtrWordFromIOVA(chain.firstIOVA32, z);
    if (cmdPtr == 0) {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);
        Base::Transition(State::ERROR, txid, "invalid_cmdptr");
        return kIOReturnBadArgument;
    }

    // Program the context and update the ring in one section with the
    // completion scan, so it sees either the ring before this packet (and may
    // stop the drained context first) or the armed packet in it.
    {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);
        ctx().WriteCommandPtr(cmdPtr);
        ctx().WriteControlSet(kContextControlRunBit);

        ASFW_LOG_V3(Async, "ctx=%{public}s txid=%u gen=%u P1_ARM head=%lu tail=%lu z=%u cmdPtr=0x%08x", RoleTag::kContextName, txid, generation_, (unsigned long)ring().Head(), (unsigned long)ring().Tail(), z, cmdPtr);

        trace_.push({NowNs(), txid, generation_, ATEvent::P1_ARM, cmdPtr, z});

        Base::Transition(State::RUNNING, txid, "path1_armed");
        UpdateRingTail_(chain);
    }

    return kIOReturnSuccess;
}

template<typename ContextT, typename RingT, typename RoleTag>
kern_return_t ATManager<ContextT, RingT, RoleTag>::SubmitPath2Locked_(const DescriptorChain& chain, uint32_t txid) {
    // PATH 2: Hot-append to running context (Apple's fire-and-forget pattern).
    // The caller holds the lock: link, WAKE and tail update are one step for
    // the completion scan. WAKE is pulsed without polling.
    const kern_return_t linkResult = LinkTailTo_(chain);
    if (linkResult != kIOReturnSuccess) {
        ASFW_LOG_V2(Async,
                    "ctx=%{public}s txid=%u gen=%u P2_FALLBACK cause=%{public}s",
                    RoleTag::kContextName,
                    txid,
                    generation_,
                    "LinkTailTo");
        trace_.push({NowNs(), txid, generation_, ATEvent::P2_FALLBACK, 0, 0});
        return linkResult;
    }
    PublishPrevLast_(chain);
    this->IoWriteFence();

    // WAKE guard: Check RUN==1 && DEAD==0 before pulsing WAKE
    const uint32_t ctrl = ctx().ReadControl();
    const bool run = (ctrl & kContextControlRunBit) != 0;
    const bool dead = (ctrl & kContextControlDeadBit) != 0;

    ASFW_LOG_V3(Async, "ctx=%{public}s txid=%u gen=%u WAKE_GUARD ctrl=0x%08x run=%d dead=%d", RoleTag::kContextName, txid, generation_, ctrl, run ? 1 : 0, dead ? 1 : 0);

    if (!run || dead) {
        // Anomaly at default level: a hot-append hitting a stopped/dead context
        // is the suspected AT-resp loss window (LS-9000 wedge investigation).
        ASFW_LOG(Async, "ctx=%{public}s txid=%u gen=%u P2_FALLBACK cause=%{public}s ctrl=0x%08x", RoleTag::kContextName, txid, generation_, !run ? "RUN0" : "DEAD", ctrl);
        UnlinkTail_();
        trace_.push({NowNs(), txid, generation_, ATEvent::P2_FALLBACK, ctrl, 0});
        return kIOReturnNotReady;
    }

    // Pulse WAKE bit and return immediately (Apple's fire-and-forget pattern)
    // From decompilation @ 0xDBBE: WAKE is a hint, hardware picks up branch asynchronously
    // NO POLLING - Apple never polls ACTIVE after WAKE in PATH-2!
    ctx().WriteControlSet(kContextControlWakeBit);

    ASFW_LOG_V3(Async, "ctx=%{public}s txid=%u gen=%u P2_WAKE pulsed", RoleTag::kContextName, txid, generation_);
    trace_.push({NowNs(), txid, generation_, ATEvent::P2_WAKE, 0, 0});

    // Let the interrupt handler (ScanCompletion) stop the context when the
    // ring drains: it takes this same lock.
    UpdateRingTail_(chain);
    return kIOReturnSuccess;
}

template<typename ContextT, typename RingT, typename RoleTag>
uint64_t ATManager<ContextT, RingT, RoleTag>::HeadStalledNs(uint64_t nowNs) noexcept {
    IOLockWrapper lockWrapper(lock());
    ScopedLock guard(lockWrapper);
    const bool observedRecently = stallLastCallNs_ != 0 && nowNs >= stallLastCallNs_ &&
                                  nowNs - stallLastCallNs_ <= kStallObservationGapNs;
    stallLastCallNs_ = nowNs;
    if (ring().IsEmpty()) {
        stallSinceNs_ = 0;
        stallRecoveredWithoutProgress_ = false;
        return 0;
    }
    const size_t head = ring().Head();
    if (stallSinceNs_ == 0 || head != stallHead_ || !observedRecently) {
        if (head != stallHead_) {
            stallRecoveredWithoutProgress_ = false;
        }
        stallHead_ = head;
        stallSinceNs_ = nowNs;
        return 0;
    }
    return nowNs - stallSinceNs_;
}

template<typename ContextT, typename RingT, typename RoleTag>
bool ATManager<ContextT, RingT, RoleTag>::StopStalledContext(uint64_t stalledNs) noexcept {
    const uint32_t ctrl = ctx().ReadControl();
    const uint32_t cmdPtrReg = ctx().ReadCommandPtr();
    bool firstForThisHead = false;
    {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);
        firstForThisHead = !stallRecoveredWithoutProgress_;
        if (firstForThisHead) {
            ASFW_LOG_WARNING(Async,
                             "[ATStall] ctx=%{public}s no packet completed for %llu ms with head=%zu tail=%zu "
                             "prevLast=%u state=%{public}s ctrl=0x%08x cmdPtr=0x%08x: restarting the context",
                             RoleTag::kContextName, stalledNs / 1'000'000ULL, ring().Head(), ring().Tail(),
                             ring().PrevLastBlocks(), ToString(this->state_), ctrl, cmdPtrReg);
        } else {
            ASFW_LOG_DEBUG(Async,
                           "[ATStall] ctx=%{public}s still no progress at head=%zu after a restart (%llu ms)",
                           RoleTag::kContextName, ring().Head(), stalledNs / 1'000'000ULL);
        }
        Base::Transition(State::STOPPING, 0, "stall");
    }
    if (!wedgeDumped_) {
        wedgeDumped_ = true;
        logWedgeSnapshot_(0, cmdPtrReg);
    }
    clearRunAndPoll_();
    if (!ctx().IsActive()) {
        return true;
    }
    if (firstForThisHead) {
        ASFW_LOG_WARNING(Async, "[ATStall] ctx=%{public}s stayed ACTIVE after RUN was cleared (ctrl=0x%08x)",
                         RoleTag::kContextName, ctx().ReadControl());
    }
    return false;
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::FinishStallRecovery() noexcept {
    IOLockWrapper lockWrapper(lock());
    ScopedLock guard(lockWrapper);
    stallSinceNs_ = 0;
    // A ring still not empty is the same episode: its next restart logs at
    // debug, without another snapshot.
    const bool drained = ring().IsEmpty();
    stallRecoveredWithoutProgress_ = !drained;
    if (drained) {
        wedgeDumped_ = false;
    }
    // What the stopped context never sent was released by the drain; the next
    // submit arms it afresh (PATH 1).
    ring().SetPrevLastBlocks(0);
    Base::Transition(State::IDLE, 0, "stall_recovered");
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::RequestStop(uint32_t txid, const char* why) noexcept {
    // Phase 1.2: Use ScopedLock for automatic RAII
    IOLockWrapper lockWrapper(lock());
    ScopedLock guard(lockWrapper);
    requestStop_(txid, why);
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::requestStop_(uint32_t txid, const char* why) noexcept {
    if (this->state_ != State::RUNNING) {
        ASFW_LOG_V2(Async, "ctx=%{public}s txid=%u gen=%u STOP_SKIP state=%{public}s", RoleTag::kContextName, txid, generation_, ToString(this->state_));
        return;
    }

    Base::Transition(State::STOPPING, txid, why);
    const auto t0 = NowUs();

    ctx().WriteControlClear(kContextControlRunBit);
    IODelay(1);
    this->IoReadFence();

    // FIX: Don't poll for ACTIVE=0 with lock held - causes deadlock!
    // Apple's pattern: Fire-and-forget, interrupt handler detects quiescence
    // Polling here blocks interrupt handler from acquiring lock to drain completions
    // Result: Hardware ACTIVE never clears because completion isn't drained → deadlock
    // for (uint32_t i = 0; i < 250; ++i) {
    //     if (!ctx().IsActive()) break;
    //     IODelay(1);
    // }

    const auto elapsed = NowUs() - t0;
    
    // Verify ring is empty before rotation
    if (ring().Head() != ring().Tail()) {
        ASFW_LOG_ERROR(Async, "[%{public}s] STOP: Ring not empty (head=%zu tail=%zu)",
                       RoleTag::kContextName, ring().Head(), ring().Tail());
    }

    rotateRingBy2_();
    ring().SetPrevLastBlocks(0);
    ++generation_;

    ASFW_LOG_V2(Async, "ctx=%{public}s txid=%u gen=%u STOP_IMM why=%{public}s elapsed_us=%lu gen=%u", RoleTag::kContextName, txid, generation_, why, (unsigned long)elapsed, generation_);
    trace_.push({NowNs(), txid, generation_, ATEvent::STOP_IMM, static_cast<uint32_t>(elapsed), generation_});

    Base::Transition(State::IDLE, txid, "stopped");
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::logWedgeSnapshot_(uint32_t txid, uint32_t cmdPtrReg) noexcept {
    const size_t capacity = ring().Capacity();
    auto dumpBlock = [&](const char* tag, size_t index) {
        const auto* d = ring().At(index);
        if (!d) return;
        uint32_t q[4];
        memcpy(q, d, sizeof(q));
        ASFW_LOG(Async, "  [wedge] %{public}s idx=%zu q0=0x%08x q1=0x%08x q2=0x%08x q3=0x%08x",
                 tag, index, q[0], q[1], q[2], q[3]);
    };
    ASFW_LOG_ERROR(Async, "=== ctx=%{public}s txid=%u WEDGE SNAPSHOT (cmdPtr=0x%08x head=%zu tail=%zu cap=%zu) ===",
                   RoleTag::kContextName, txid, cmdPtrReg, ring().Head(), ring().Tail(), capacity);
    if (capacity > 0) {
        if (const auto idx = ring().IndexFromIOVA(cmdPtrReg)) {
            for (size_t k = 0; k < 4; ++k) {
                dumpBlock(k < 2 ? "cmdPtr-" : "cmdPtr+", (*idx + capacity - 2 + k) % capacity);
            }
        } else {
            ASFW_LOG(Async, "  [wedge] cmdPtr not inside ring");
        }
        for (size_t k = 0; k < 3; ++k) {
            dumpBlock("head-", (ring().Head() + capacity - 2 + k) % capacity);
        }
    }
    trace_.dump();
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::clearRunAndPoll_() noexcept {
    ctx().WriteControlClear(kContextControlRunBit);
    IODelay(1);
    this->IoReadFence();
    // Escalating wait (Apple waitForDMA pattern, ~32 ms bound) — a 250 µs poll
    // gave up while the previous packet was still on the wire.
    (void)ctx().WaitForQuiesce();
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::rotateRingBy2_() noexcept {
    const size_t capacity = ring().Capacity();
    if (capacity == 0) return;
    
    const size_t currentHead = ring().Head();
    const size_t newHead = (currentHead + 2) % capacity;
    ring().SetHead(newHead);
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::PublishChain_(const DescriptorChain& chain) {
    builder_.FlushChain(chain);
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::UpdateRingTail_(const DescriptorChain& chain) {
    const size_t newTail = (chain.lastRingIndex + 1) % ring().Capacity();
    // The allocator may skip the final one/two slots to keep a packet contiguous.
    // When arming an empty ring, the first live packet is the allocation start.
    if (ring().IsEmpty()) {
        ring().SetHead(chain.firstRingIndex);
    }
    ring().SetTail(newTail);
    // DescriptorRing::LocatePreviousLast() walks back TotalBlocks() (2 = immediate
    // only, 3 = immediate header + payload) from the tail to find the previous
    // OUTPUT_LAST. Storing lastBlocks (1 for a payload packet) made every PATH2
    // hot-append fail for Z=3 packets, forcing PATH1 re-arms that raced the
    // in-flight packet (LS-9000 AT Response wedge, 2026-09-24).
    ring().SetPrevLastBlocks(chain.TotalBlocks());
}

template<typename ContextT, typename RingT, typename RoleTag>
kern_return_t ATManager<ContextT, RingT, RoleTag>::LinkTailTo_(const DescriptorChain& chain) {
    return builder_.LinkTailTo(ring().Tail(), chain) ? kIOReturnSuccess : kIOReturnNotReady;
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::UnlinkTail_() noexcept {
    builder_.UnlinkTail(ring().Tail());
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::PublishPrevLast_(const DescriptorChain& chain) {
    const uint8_t prevBlocks = ring().PrevLastBlocks();
    const size_t tailIndex = ring().Tail();
    
    // Locate previous LAST descriptor to determine if it's immediate
    HW::OHCIDescriptor* prevLast = nullptr;
    size_t prevLastIndex = 0;
    uint8_t blocks = 0;
    bool prevIsImmediate = false;
    
    if (ring().LocatePreviousLast(tailIndex, prevLast, prevLastIndex, blocks)) {
        prevIsImmediate = HW::IsImmediate(*prevLast);
        // Use the actual number of blocks for the previous LAST descriptor
        builder_.FlushTail(prevLastIndex, blocks);
        return;
    }

    // Fallback: flush using cached prevBlocks (should not typically happen)
    builder_.FlushTail(prevLastIndex, static_cast<uint8_t>(prevBlocks));
}

} // namespace ASFW::Async::Engine
