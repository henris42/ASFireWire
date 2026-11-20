//
// IsochTypes.hpp
// ASFWDriver
//
// Core Isochronous Type Definitions and OHCI context definitions.
//

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <TargetConditionals.h>

#include "IsochLinkCounters.hpp"
#include "IsochReceivePollSource.hpp"

namespace ASFW::Isoch {

// A single OHCI drain obtains one controller-cycle/host-time pair. Every
// packet completed by that drain is correlated to this pair by its consumer;
// the transport deliberately does not interpret packet content.
struct IsochReceiveBatch final {
    uint32_t drainCycleTimer{0};
    uint64_t drainHostTicks{0};
    // Drained on a reader's thread, on demand (TryPoll: the audio side's
    // BeginRead pull on the real-time IO thread), not by the interrupt or the
    // watchdog. A consumer keeps that batch to its own state and leaves side
    // effects that reach other subsystems to the next regular poll.
    bool readerPull{false};
    // The stream's link counters, for what only the content side can tell
    // (block continuity, missing cycles). Valid for the batch; may be null.
    IsochRxLinkCounters* link{nullptr};
};

// Payload-opaque result of one IR descriptor. `payload` is valid only for the
// duration of the synchronous ConsumePacket() call; consumers must copy any
// data they need after that point.
struct IsochReceivePacket final {
    uint32_t descriptorIndex{0};
    uint16_t transferStatus{0};
    uint16_t residualCount{0};
    std::span<const uint8_t> payload{};
};

// Content consumers live outside transport. The receive context invokes these
// methods synchronously on its polling path and never reads or writes
// consumer-owned state itself.
class IIsochReceiveConsumer {
  public:
    virtual ~IIsochReceiveConsumer() = default;

    // Called only after the context has armed its DMA ring, and only after the
    // context has quiesced it.  A consumer may release payload-derived views
    // from OnReceiveQuiesced(); it must not retain `payload` past ConsumePacket.
    virtual void OnReceiveActivated() noexcept {}
    virtual void OnReceiveQuiesced() noexcept {}

    // The context this consumer is attached to, as something it may poll
    // from another thread: set before OnReceiveActivated(), cleared (nullptr)
    // after OnReceiveQuiesced() and before the context goes away. A consumer
    // that handed the source to another thread must make sure that thread is
    // done with it before this returns.
    virtual void SetReceivePollSource(IIsochReceivePollSource* source) noexcept { (void)source; }

    virtual void BeginReceiveBatch(const IsochReceiveBatch& batch) noexcept = 0;
    virtual void ConsumePacket(const IsochReceiveBatch& batch,
                               const IsochReceivePacket& packet) noexcept = 0;
    // After the last packet of the drain, once per BeginReceiveBatch. A
    // consumer that hands work to another queue does it here, so everything the
    // batch carried is already published when that queue looks. Payload-opaque
    // like the rest of this interface.
    virtual void EndReceiveBatch(const IsochReceiveBatch&) noexcept {}

    // Off-hot-path observability hooks.  They are optional because content
    // formats decide which timing and payload facts are meaningful.
    virtual void DrainReceiveTelemetry(uint32_t) {}
    // Periodic, off-hot-path service for consumer-owned diagnostics (e.g. a
    // consumer reporting errors its own real-time path recorded).
    virtual void ServiceConsumerDiagnostics() {}
};

// Callback for received packets (Raw transport level)
// @param data: Span containing packet data (header + payload)
// @param status: Status bits from descriptor
// @param timestamp: Timestamp of reception
using IsochReceiveCallback = std::function<void(std::span<const uint8_t> data, uint32_t status, uint64_t timestamp)>;

} // namespace ASFW::Isoch
