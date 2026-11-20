// LinkReporter.cpp

#include "LinkReporter.hpp"

#include "../Async/Interfaces/IAsyncSubsystemPort.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Isoch/IsochReceiveContext.hpp"
#include "../Isoch/Transmit/IsochTransmitContext.hpp"
#include "../Logging/Logging.hpp"

namespace ASFW::Diagnostics {
namespace {

constexpr size_t kLineBytes = 224;

// A counter that moved is always worth a line; the rest is for `info`.
[[nodiscard]] bool Wanted(bool moved) noexcept {
    return moved ? ASFW_LOG_ENABLED(Metrics, Warning) : ASFW_LOG_ENABLED(Metrics, Info);
}

void Emit(bool moved, const char* line) noexcept {
    if (moved) {
        ASFW_LOG_WARNING(Metrics, "%{public}s", line);
    } else {
        ASFW_LOG_INFO(Metrics, "%{public}s", line);
    }
}

[[nodiscard]] Link::TxFigures TxFiguresFrom(const ::ASFW::Isoch::IsochTransmitContext& context) noexcept {
    const auto& c = context.RingCounters();
    Link::TxFigures f;
    f.packets = c.completedPackets.load(std::memory_order_relaxed);
    f.statusErrors = c.abnormalCompletions.load(std::memory_order_relaxed);
    f.cyclesMissed = c.lostCycles.load(std::memory_order_relaxed);
    f.lastBadStatus = c.lastAbnormalStatus.load(std::memory_order_relaxed);
    return f;
}

void EmitRx(uint64_t guid, const char* span, const Link::RxFigures& figures) noexcept {
    const bool moved = figures.AnyError();
    if (!Wanted(moved)) {
        return;
    }
    char line[kLineBytes];
    Link::FormatRx(line, sizeof(line), guid, span, figures);
    Emit(moved, line);
}

void EmitTx(uint64_t guid, const char* span, const Link::TxFigures& figures) noexcept {
    const bool moved = figures.AnyError();
    if (!Wanted(moved)) {
        return;
    }
    char line[kLineBytes];
    Link::FormatTx(line, sizeof(line), guid, span, figures);
    Emit(moved, line);
}

} // namespace

void LinkReporter::Tick(uint64_t nowNs, ::ASFW::Driver::IsochLanes& lanes,
                        const ::ASFW::Driver::ControllerCore* controller,
                        const ::ASFW::Async::IAsyncSubsystemPort* async) noexcept {
    if (nowNs < nextPollNs_) {
        return;
    }
    nextPollNs_ = nowNs + kPollNs;

    bool streaming = false;
    for (uint32_t lane = 0; lane < units_.size(); ++lane) {
        streaming = PollUnit(units_[lane], lane, nowNs, lanes) || streaming;
    }

    if (lastBusReportNs_ == 0) {
        lastBusReportNs_ = nowNs;
    } else if (nowNs - lastBusReportNs_ >= kReportNs) {
        ReportBus(nowNs, streaming, controller, async);
        lastBusReportNs_ = nowNs;
    }
}

bool LinkReporter::PollUnit(Unit& unit, uint32_t lane, uint64_t nowNs,
                            ::ASFW::Driver::IsochLanes& lanes) noexcept {
    const auto* service = lanes.Lane(lane);
    if (service == nullptr) {
        return false;
    }
    // The lane forgets its unit when it stops; the totals are written after.
    if (const uint64_t active = service->ActiveGuid(); active != 0) {
        unit.guid.store(active, std::memory_order_relaxed);
    }
    const uint64_t guid = unit.guid.load(std::memory_order_relaxed);

    const uint32_t index = lane * ::ASFW::Driver::IsochLanes::kLaneWidth;
    const auto* receive = lanes.ReceiveContext(index);
    const auto* transmit = lanes.TransmitContext(index);
    const bool rxRunning =
        receive != nullptr && receive->GetState() == ::ASFW::Isoch::IRPolicy::State::Running;
    const bool txRunning =
        transmit != nullptr && transmit->GetState() == ::ASFW::Isoch::ITState::Running;

    if ((rxRunning || txRunning) && !(unit.rxRunning || unit.txRunning)) {
        unit.lastReportNs = nowNs;
    }

    // A start zeroes the context's counters; a stop leaves them standing
    // until the next start.
    if (rxRunning && !unit.rxRunning) {
        unit.rx = {};
    } else if (!rxRunning && unit.rxRunning && receive != nullptr) {
        EmitRx(guid, "stream", Link::RxFigures::From(receive->LinkCounters()));
    }
    if (txRunning && !unit.txRunning) {
        unit.tx = {};
    } else if (!txRunning && unit.txRunning && transmit != nullptr) {
        EmitTx(guid, "stream", TxFiguresFrom(*transmit));
    }
    unit.rxRunning = rxRunning;
    unit.txRunning = txRunning;

    if ((rxRunning || txRunning) && nowNs - unit.lastReportNs >= kReportNs) {
        char span[24];
        Link::FormatSpan(span, sizeof(span), nowNs - unit.lastReportNs);
        if (rxRunning) {
            const auto now = Link::RxFigures::From(receive->LinkCounters());
            EmitRx(guid, span, now.Since(unit.rx));
            unit.rx = now;
        }
        if (txRunning) {
            const auto now = TxFiguresFrom(*transmit);
            EmitTx(guid, span, now.Since(unit.tx));
            unit.tx = now;
        }
        unit.lastReportNs = nowNs;
    }
    return rxRunning || txRunning;
}

void LinkReporter::ReportBus(uint64_t nowNs, bool streaming,
                             const ::ASFW::Driver::ControllerCore* controller,
                             const ::ASFW::Async::IAsyncSubsystemPort* async) noexcept {
    char span[24];
    Link::FormatSpan(span, sizeof(span), nowNs - lastBusReportNs_);

    if (controller != nullptr) {
        const auto now = Link::ControllerFigures::From(controller->LinkCounters());
        const auto delta = now.Since(controller_);
        controller_ = now;
        const bool moved = delta.AnyError();
        // With nothing streaming and nothing moved the line says nothing.
        if ((moved || streaming) && Wanted(moved)) {
            char line[kLineBytes];
            Link::FormatController(line, sizeof(line), span, delta);
            Emit(moved, line);
        }
    }

    const auto* counters = async != nullptr ? async->GetLinkCounters() : nullptr;
    if (counters == nullptr) {
        return;
    }
    for (uint32_t node = 0; node < async_.size(); ++node) {
        const auto now = Link::AsyncNodeFigures::From(counters->node[node]);
        const auto delta = now.Since(async_[node]);
        async_[node] = now;
        if (!delta.AnyTraffic()) {
            continue;
        }
        const bool moved = delta.AnyError();
        if (!Wanted(moved)) {
            continue;
        }
        char line[kLineBytes];
        Link::FormatAsync(line, sizeof(line), node, span, delta);
        Emit(moved, line);
    }
}

void LinkReporter::Collect(Link::StatsWire& out, ::ASFW::Driver::IsochLanes& lanes,
                           const ::ASFW::Driver::ControllerCore* controller,
                           const ::ASFW::Async::IAsyncSubsystemPort* async) const noexcept {
    if (controller != nullptr) {
        out.SetController(Link::ControllerFigures::From(controller->LinkCounters()));
    }

    for (uint32_t lane = 0; lane < units_.size(); ++lane) {
        if (lanes.Lane(lane) == nullptr) {
            continue;
        }
        const uint32_t index = lane * ::ASFW::Driver::IsochLanes::kLaneWidth;
        const auto* receive = lanes.ReceiveContext(index);
        const auto* transmit = lanes.TransmitContext(index);
        const auto rx = receive != nullptr ? Link::RxFigures::From(receive->LinkCounters()) : Link::RxFigures{};
        const auto tx = transmit != nullptr ? TxFiguresFrom(*transmit) : Link::TxFigures{};
        // A lane that never streamed has nothing to show.
        if (rx.packets == 0 && tx.packets == 0) {
            continue;
        }
        const bool rxRuns =
            receive != nullptr && receive->GetState() == ::ASFW::Isoch::IRPolicy::State::Running;
        const bool txRuns =
            transmit != nullptr && transmit->GetState() == ::ASFW::Isoch::ITState::Running;
        (void)out.AddUnit(units_[lane].guid.load(std::memory_order_relaxed), rxRuns, txRuns, rx, tx);
    }

    if (const auto* counters = async != nullptr ? async->GetLinkCounters() : nullptr; counters != nullptr) {
        for (uint32_t node = 0; node < ::ASFW::Async::AsyncLinkCounters::kNodes; ++node) {
            const auto figures = Link::AsyncNodeFigures::From(counters->node[node]);
            if (figures.AnyTraffic()) {
                (void)out.AddAsyncNode(node, figures);
            }
        }
    }

    // The local PHY's port status page (IEEE 1394a-2000 annex J: register 7
    // selects page and port, 8 onwards are the port's). Each access waits on
    // the PHY for a millisecond or two, so not while a stream runs.
    auto* hardware = controller != nullptr ? controller->GetHardware() : nullptr;
    if (hardware == nullptr) {
        out.SetPhyState(Link::StatsWire::kPhyUnreadable);
        return;
    }
    if (hardware->IsIsochStreamingActive()) {
        out.SetPhyState(Link::StatsWire::kPhySkippedStreaming);
        return;
    }
    constexpr uint8_t kPortCountRegister = 2;
    constexpr uint8_t kSelectRegister = 7;
    const auto portCount = hardware->ReadPhyRegister(kPortCountRegister);
    const auto selected = hardware->ReadPhyRegister(kSelectRegister);
    if (!portCount.has_value() || !selected.has_value()) {
        out.SetPhyState(Link::StatsWire::kPhyUnreadable);
        return;
    }
    const uint8_t ports = *portCount & 0x1FU;
    for (uint8_t port = 0; port < ports && port < Link::StatsWire::kMaxPhyPorts; ++port) {
        if (!hardware->WritePhyRegister(kSelectRegister, port)) {
            out.SetPhyState(Link::StatsWire::kPhyUnreadable);
            break;
        }
        std::array<uint8_t, 4> regs{};
        bool read = true;
        for (uint8_t reg = 0; reg < regs.size(); ++reg) {
            const auto value = hardware->ReadPhyRegister(static_cast<uint8_t>(8 + reg));
            if (!value.has_value()) {
                read = false;
                break;
            }
            regs[reg] = *value;
        }
        if (!read) {
            out.SetPhyState(Link::StatsWire::kPhyUnreadable);
            break;
        }
        (void)out.AddPhyPort(port, regs[0], regs[1], regs[2], regs[3]);
    }
    (void)hardware->WritePhyRegister(kSelectRegister, *selected);
}

} // namespace ASFW::Diagnostics
