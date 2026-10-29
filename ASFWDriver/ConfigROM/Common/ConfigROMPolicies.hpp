#pragma once

#include <cstddef>
#include <cstdint>

#include "../../Async/AsyncTypes.hpp"
#include "../../Discovery/SpeedPolicy.hpp"
#include "../Remote/ROMScanNodeStateMachine.hpp"
#include "ConfigROMConstants.hpp"

namespace ASFW::Discovery {

class GenerationContextPolicy {
  public:
    [[nodiscard]] static constexpr bool IsCurrentEvent(Generation eventGeneration,
                                                       Generation activeGeneration) noexcept {
        return eventGeneration != Generation{0} && eventGeneration == activeGeneration;
    }

    [[nodiscard]] static constexpr bool
    CanRestartIdleScan(Generation activeGeneration, bool scannerIdle,
                       Generation requestedGeneration) noexcept {
        return scannerIdle && requestedGeneration != Generation{0} &&
               requestedGeneration != activeGeneration;
    }

    [[nodiscard]] static constexpr bool MatchesActiveScan(Generation requestedGeneration,
                                                          Generation activeGeneration) noexcept {
        return requestedGeneration == activeGeneration;
    }
};

class ShortReadResolutionPolicy {
  public:
    [[nodiscard]] static constexpr bool IsValidQuadletPayload(size_t payloadSizeBytes) noexcept {
        return payloadSizeBytes == ASFW::ConfigROM::kQuadletBytes;
    }

    [[nodiscard]] static constexpr bool ShouldTreatAsEOF(Async::AsyncStatus status,
                                                         size_t payloadSizeBytes,
                                                         uint32_t completedQuadlets) noexcept {
        return completedQuadlets > 0 &&
               (status != Async::AsyncStatus::kSuccess || !IsValidQuadletPayload(payloadSizeBytes));
    }

    [[nodiscard]] static constexpr bool IsReadFailure(Async::AsyncStatus status,
                                                      size_t payloadSizeBytes,
                                                      uint32_t completedQuadlets) noexcept {
        return !ShouldTreatAsEOF(status, payloadSizeBytes, completedQuadlets) &&
               (status != Async::AsyncStatus::kSuccess || !IsValidQuadletPayload(payloadSizeBytes));
    }

    [[nodiscard]] static constexpr uint16_t
    ClampHeaderFirstEntryCount(uint16_t entryCount) noexcept {
        if (entryCount > ASFW::ConfigROM::kHeaderFirstMaxEntries) {
            return static_cast<uint16_t>(ASFW::ConfigROM::kHeaderFirstMaxEntries);
        }
        return entryCount;
    }
};

// Retries a failed scan step at the node's own speed, never lower: a node that
// cannot be read at its path speed is left unconnected (b152: stepping S400 ->
// S200 -> S100 on response timeouts connected a slow-booting unit at S100,
// where it streamed at four times the bandwidth). The patience for slow units
// is the delayed Config ROM readiness retries, not a lower speed.
class RetryBackoffPolicy {
  public:
    enum class Decision : uint8_t {
        RetrySameSpeed,
        FailedExhausted,
    };

    template <typename TransitionFn>
    [[nodiscard]] Decision Apply(ROMScanNodeStateMachine& node, SpeedPolicy& speedPolicy,
                                 TransitionFn&& transitionNodeState) const {
        if (node.RetriesLeft() > 0) {
            node.DecrementRetries();
            transitionNodeState(node, ROMScanNodeStateMachine::State::Idle,
                                "RetryAtSameSpeed retry");
            return Decision::RetrySameSpeed;
        }

        speedPolicy.RecordTimeout(node.NodeId(), node.CurrentSpeed());
        transitionNodeState(node, ROMScanNodeStateMachine::State::Failed,
                            "RetryAtSameSpeed exhausted retries");
        return Decision::FailedExhausted;
    }
};

} // namespace ASFW::Discovery
