#include "SpeedPolicy.hpp"
#include "DiscoveryValues.hpp"  // For MaxPayload constants
#include "../Logging/Logging.hpp"

namespace ASFW::Discovery {

namespace {
// Scoped IOLock, matching DeviceRegistry's idiom in this layer.
class LockGuard final {
  public:
    explicit LockGuard(IOLock* lock) noexcept : lock_(lock) {
        if (lock_) IOLockLock(lock_);
    }
    ~LockGuard() {
        if (lock_) IOLockUnlock(lock_);
    }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

  private:
    IOLock* lock_;
};
} // namespace

SpeedPolicy::SpeedPolicy() : lock_(IOLockAlloc()) {}

SpeedPolicy::~SpeedPolicy() {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

namespace {
uint32_t SpeedMbps(FwSpeed speed) {
    return 100u << static_cast<uint8_t>(speed);
}
} // namespace

LinkPolicy SpeedPolicy::ForNode(uint8_t nodeId) const {
    LockGuard guard(lock_);
    LinkPolicy policy{};
    
    auto it = nodeStates_.find(nodeId);
    if (it != nodeStates_.end()) {
        policy.localToNode = it->second.currentSpeed;
    } else {
        policy.localToNode = FwSpeed::S400;
    }
    
    // SpeedPolicy has no topology, so it cannot answer the isochronous question.
    // Seed it with the async speed so a caller that never learns the Self-ID
    // path speed degrades to today's behaviour rather than to S100; discovery
    // overwrites this with the real path speed (ControllerCoreDiscovery.cpp).
    policy.isochToNode = policy.localToNode;

    policy.maxPayloadBytes = ComputeMaxPayload(policy.localToNode);
    policy.halvePackets = halfSizePackets_;
    
    return policy;
}

std::optional<FW::FwSpeed> SpeedPolicy::ObservedSpeed(FW::NodeId nodeId) const noexcept {
    LockGuard guard(lock_);
    const auto it = nodeStates_.find(nodeId.value);
    if (it == nodeStates_.end()) {
        return std::nullopt;
    }
    // A node present in the map has been transacted with at least once, so its
    // currentSpeed reflects real outcomes rather than the constructor seed.
    return it->second.currentSpeed;
}

void SpeedPolicy::RecordVerifiedCeiling(FW::NodeId nodeId, FW::FwSpeed speed) noexcept {
    // Speeds are never lowered: a node runs at its path speed or not at all
    // (b152: a unit slow to answer at boot was read at S100 and then streamed
    // at S100, four times the bandwidth). The node is only marked as seen.
    (void)speed;
    LockGuard guard(lock_);
    (void)nodeStates_[nodeId.value];
}

void SpeedPolicy::RecordSuccess(uint8_t nodeId, FwSpeed speed) {
    uint8_t successCount = 0;
    {
        LockGuard guard(lock_);
        auto& state = nodeStates_[nodeId];
        state.successCount++;
        // Reset timeout counter on success
        state.timeoutCount = 0;
        successCount = state.successCount;
    }

    // Logging stays outside the lock (DeviceRegistry does the same). ObservedSpeed()
    // is now called from other queues on the transaction path, so the hold time
    // here is somebody else's latency.
    ASFW_LOG_RL(Discovery, "speed_success", 5000, Debug,
                "Node %u: Success at S%u (total=%u)",
                nodeId, SpeedMbps(speed), successCount);
}

void SpeedPolicy::RecordTimeout(uint8_t nodeId, FwSpeed speed) {
    // ROMScanSession calls this once a node's retries at its speed are spent.
    // The speed is never lowered: a late or missing answer is not a reason to
    // drop speed, and a node that cannot be read at its path speed is better
    // left unconnected than connected at S100 (b152). Only counted, for the log.
    uint8_t timeoutCount = 0;
    {
        LockGuard guard(lock_);
        auto& state = nodeStates_[nodeId];
        state.timeoutCount++;
        timeoutCount = state.timeoutCount;
    }

    // Logging stays outside the lock (DeviceRegistry does the same). ObservedSpeed()
    // is now called from other queues on the transaction path, so the hold time
    // here is somebody else's latency.
    ASFW_LOG(Discovery, "Node %u: Timeout at S%u (count=%u); speed kept",
             nodeId, SpeedMbps(speed), timeoutCount);
}

void SpeedPolicy::SetHalfSizePackets(bool enabled) {
    LockGuard guard(lock_);
    halfSizePackets_ = enabled;
}

void SpeedPolicy::Reset() {
    LockGuard guard(lock_);
    nodeStates_.clear();
}

uint16_t SpeedPolicy::ComputeMaxPayload(FwSpeed speed) const {
    uint16_t basePayload = 0;
    
    switch (speed) {
        case FwSpeed::S100: basePayload = MaxPayload::kS100; break;
        case FwSpeed::S200: basePayload = MaxPayload::kS200; break;
        case FwSpeed::S400: basePayload = MaxPayload::kS400; break;
        case FwSpeed::S800: basePayload = MaxPayload::kS800; break;
    }
    
    if (halfSizePackets_) {
        basePayload /= 2;
    }
    
    return basePayload;
}

} // namespace ASFW::Discovery
