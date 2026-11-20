// LinkReporter.hpp
// ASFW - the [Link] lines: what the link, the packets and the bus did wrong
// in the last interval, per streaming unit, for the controller and per node.

#pragma once

#include "LinkReport.hpp"
#include "LinkStatsWire.hpp"

#include "../Isoch/IsochLanes.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Async {
class IAsyncSubsystemPort;
}

namespace ASFW::Driver {
class ControllerCore;
}

namespace ASFW::Diagnostics {

// Called from the watchdog tick. A line is written at info every 5 s, and at
// warn whenever a counter that should stay at zero moved; a unit's totals are
// written once when its stream stops. Nothing here runs on an IO thread: the
// counters are written where the packets are handled and only read here.
class LinkReporter final {
  public:
    static constexpr uint64_t kPollNs = 100'000'000ULL;
    static constexpr uint64_t kReportNs = 5'000'000'000ULL;

    void Tick(uint64_t nowNs, ::ASFW::Driver::IsochLanes& lanes,
              const ::ASFW::Driver::ControllerCore* controller,
              const ::ASFW::Async::IAsyncSubsystemPort* async) noexcept;

    // The totals for the user client, read where they stand; any thread.
    // Reads the local PHY's port registers unless a stream runs.
    void Collect(Link::StatsWire& out, ::ASFW::Driver::IsochLanes& lanes,
                 const ::ASFW::Driver::ControllerCore* controller,
                 const ::ASFW::Async::IAsyncSubsystemPort* async) const noexcept;

  private:
    struct Unit final {
        std::atomic<uint64_t> guid{0};
        uint64_t lastReportNs{0};
        bool rxRunning{false};
        bool txRunning{false};
        Link::RxFigures rx{};
        Link::TxFigures tx{};
    };

    [[nodiscard]] bool PollUnit(Unit& unit, uint32_t lane, uint64_t nowNs,
                                ::ASFW::Driver::IsochLanes& lanes) noexcept;
    void ReportBus(uint64_t nowNs, bool streaming, const ::ASFW::Driver::ControllerCore* controller,
                   const ::ASFW::Async::IAsyncSubsystemPort* async) noexcept;

    std::array<Unit, ::ASFW::Driver::IsochLanes::kMaxLanes> units_{};
    Link::ControllerFigures controller_{};
    std::array<Link::AsyncNodeFigures, ::ASFW::Async::AsyncLinkCounters::kNodes> async_{};
    uint64_t nextPollNs_{0};
    uint64_t lastBusReportNs_{0};
};

} // namespace ASFW::Diagnostics
