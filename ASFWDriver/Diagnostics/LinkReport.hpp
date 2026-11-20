// LinkReport.hpp
// ASFW - link metrics: snapshots of the counters, what moved between two of
// them, and the [Link] lines. No driver state; host-testable.

#pragma once

#include "../Async/Track/AsyncLinkCounters.hpp"
#include "../Controller/ControllerLinkCounters.hpp"
#include "../Isoch/Core/IsochLinkCounters.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ASFW::Diagnostics::Link {

// A counter that restarted (a stream start zeroes its counters) reads lower
// than before; what it holds now is then what the interval added.
[[nodiscard]] constexpr uint64_t Since(uint64_t now, uint64_t before) noexcept {
    return now >= before ? now - before : now;
}

struct RxFigures final {
    uint64_t packets{0};
    uint64_t dataCrc{0};
    uint64_t otherStatus{0}; // every status that is neither complete nor a data CRC error
    uint64_t lengthMismatch{0};
    uint64_t dbcGaps{0};
    uint64_t dbcBlocksMissed{0};
    uint64_t cyclesMissed{0};
    uint64_t cipChanges{0};
    uint32_t lastBadStatus{0};
    uint32_t speedCode{0};
    bool dbcUnusable{false};

    [[nodiscard]] static RxFigures From(const ::ASFW::Isoch::IsochRxLinkCounters& c) noexcept {
        const auto get = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
        RxFigures f;
        f.packets = get(c.packets);
        f.dataCrc = get(c.dataCrc);
        f.otherStatus = get(c.longPacket) + get(c.overrun) + get(c.descriptorRead) + get(c.dataWrite) +
                        get(c.timeout) + get(c.unknown) + get(c.otherStatus);
        f.lengthMismatch = get(c.lengthMismatch);
        f.dbcUnusable = c.dbcUnusable.load(std::memory_order_relaxed) != 0;
        f.dbcGaps = f.dbcUnusable ? 0 : get(c.dbcGaps);
        f.dbcBlocksMissed = f.dbcUnusable ? 0 : get(c.dbcBlocksMissed);
        f.cyclesMissed = get(c.cyclesMissed);
        f.cipChanges = get(c.cipChanges);
        f.lastBadStatus = c.lastBadStatus.load(std::memory_order_relaxed);
        f.speedCode = c.lastSpeedCode.load(std::memory_order_relaxed);
        return f;
    }

    [[nodiscard]] RxFigures Since(const RxFigures& before) const noexcept {
        RxFigures d = *this;
        d.packets = Link::Since(packets, before.packets);
        d.dataCrc = Link::Since(dataCrc, before.dataCrc);
        d.otherStatus = Link::Since(otherStatus, before.otherStatus);
        d.lengthMismatch = Link::Since(lengthMismatch, before.lengthMismatch);
        d.dbcGaps = Link::Since(dbcGaps, before.dbcGaps);
        d.dbcBlocksMissed = Link::Since(dbcBlocksMissed, before.dbcBlocksMissed);
        d.cyclesMissed = Link::Since(cyclesMissed, before.cyclesMissed);
        d.cipChanges = Link::Since(cipChanges, before.cipChanges);
        return d;
    }

    [[nodiscard]] bool AnyError() const noexcept {
        return (dataCrc | otherStatus | lengthMismatch | dbcGaps | cyclesMissed | cipChanges) != 0;
    }
};

struct TxFigures final {
    uint64_t packets{0};
    uint64_t statusErrors{0};
    uint64_t cyclesMissed{0};
    uint32_t lastBadStatus{0};

    [[nodiscard]] TxFigures Since(const TxFigures& before) const noexcept {
        TxFigures d = *this;
        d.packets = Link::Since(packets, before.packets);
        d.statusErrors = Link::Since(statusErrors, before.statusErrors);
        d.cyclesMissed = Link::Since(cyclesMissed, before.cyclesMissed);
        return d;
    }

    [[nodiscard]] bool AnyError() const noexcept { return (statusErrors | cyclesMissed) != 0; }
};

struct ControllerFigures final {
    uint64_t cycleLost{0};
    uint64_t cycleInconsistent{0};
    uint64_t cycleTooLong{0};
    uint64_t postedWriteErr{0};
    uint64_t regAccessFail{0};
    uint64_t unrecoverableError{0};
    uint64_t busResets{0};
    uint32_t generation{0};

    [[nodiscard]] static ControllerFigures From(const ::ASFW::Driver::ControllerLinkCounters& c) noexcept {
        const auto get = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
        ControllerFigures f;
        f.cycleLost = get(c.cycleLost);
        f.cycleInconsistent = get(c.cycleInconsistent);
        f.cycleTooLong = get(c.cycleTooLong);
        f.postedWriteErr = get(c.postedWriteErr);
        f.regAccessFail = get(c.regAccessFail);
        f.unrecoverableError = get(c.unrecoverableError);
        f.busResets = get(c.busResets);
        f.generation = c.generation.load(std::memory_order_relaxed);
        return f;
    }

    [[nodiscard]] ControllerFigures Since(const ControllerFigures& before) const noexcept {
        ControllerFigures d = *this;
        d.cycleLost = Link::Since(cycleLost, before.cycleLost);
        d.cycleInconsistent = Link::Since(cycleInconsistent, before.cycleInconsistent);
        d.cycleTooLong = Link::Since(cycleTooLong, before.cycleTooLong);
        d.postedWriteErr = Link::Since(postedWriteErr, before.postedWriteErr);
        d.regAccessFail = Link::Since(regAccessFail, before.regAccessFail);
        d.unrecoverableError = Link::Since(unrecoverableError, before.unrecoverableError);
        d.busResets = Link::Since(busResets, before.busResets);
        return d;
    }

    [[nodiscard]] bool AnyError() const noexcept {
        return (cycleLost | cycleInconsistent | cycleTooLong | postedWriteErr | regAccessFail |
                unrecoverableError | busResets) != 0;
    }
};

struct AsyncNodeFigures final {
    uint32_t requests{0};
    uint32_t ackErr{0};
    uint32_t busy{0};
    uint32_t noAck{0};
    uint32_t timeouts{0};
    uint32_t otherEvent{0};

    [[nodiscard]] static AsyncNodeFigures From(const ::ASFW::Async::AsyncNodeLinkCounters& c) noexcept {
        AsyncNodeFigures f;
        f.requests = c.requests.load(std::memory_order_relaxed);
        f.ackErr = c.ackErr.load(std::memory_order_relaxed);
        f.busy = c.busy.load(std::memory_order_relaxed);
        f.noAck = c.noAck.load(std::memory_order_relaxed);
        f.timeouts = c.timeouts.load(std::memory_order_relaxed);
        f.otherEvent = c.otherEvent.load(std::memory_order_relaxed);
        return f;
    }

    [[nodiscard]] AsyncNodeFigures Since(const AsyncNodeFigures& before) const noexcept {
        // A new async subsystem starts its counters again, as a stream start does.
        const auto since = [](uint32_t now, uint32_t was) { return now >= was ? now - was : now; };
        AsyncNodeFigures d;
        d.requests = since(requests, before.requests);
        d.ackErr = since(ackErr, before.ackErr);
        d.busy = since(busy, before.busy);
        d.noAck = since(noAck, before.noAck);
        d.timeouts = since(timeouts, before.timeouts);
        d.otherEvent = since(otherEvent, before.otherEvent);
        return d;
    }

    [[nodiscard]] bool AnyTraffic() const noexcept {
        return (requests | ackErr | busy | noAck | timeouts | otherEvent) != 0;
    }
    // A node answering busy is the node's doing, not the link's.
    [[nodiscard]] bool AnyError() const noexcept { return (ackErr | noAck | timeouts | otherEvent) != 0; }
};

[[nodiscard]] constexpr const char* SpeedName(uint32_t speedCode) noexcept {
    switch (speedCode) {
    case 0:
        return "S100";
    case 1:
        return "S200";
    case 2:
        return "S400";
    case 3:
        return "S800";
    default:
        return "S?";
    }
}

// "last 5.0 s" from an interval in nanoseconds.
inline int FormatSpan(char* out, size_t size, uint64_t intervalNs) noexcept {
    const uint64_t tenths = (intervalNs + 50'000'000ULL) / 100'000'000ULL;
    return std::snprintf(out, size, "last %llu.%llu s", static_cast<unsigned long long>(tenths / 10),
                         static_cast<unsigned long long>(tenths % 10));
}

// The lines. `span` is "last 5.0 s" or "stream"; each fits the log ring's
// message size. Returns the length written.
inline int FormatRx(char* out, size_t size, uint64_t guid, const char* span, const RxFigures& f) noexcept {
    char dbc[72];
    if (f.dbcUnusable) {
        std::snprintf(dbc, sizeof(dbc), "dbc n/a (device counts differently)");
    } else {
        std::snprintf(dbc, sizeof(dbc), "dbc gaps %llu (%llu blocks)",
                      static_cast<unsigned long long>(f.dbcGaps),
                      static_cast<unsigned long long>(f.dbcBlocksMissed));
    }
    char last[32] = "";
    if (f.dataCrc != 0 || f.otherStatus != 0) {
        std::snprintf(last, sizeof(last), " (last 0x%04x)", f.lastBadStatus & 0xFFFFU);
    }
    return std::snprintf(out, size,
                         "[Link] 0x%016llx %s: rx %llu pk %s, crc %llu, other status %llu%s, length %llu, "
                         "%s, cycles missed %llu, cip changes %llu",
                         static_cast<unsigned long long>(guid), span,
                         static_cast<unsigned long long>(f.packets), SpeedName(f.speedCode),
                         static_cast<unsigned long long>(f.dataCrc),
                         static_cast<unsigned long long>(f.otherStatus), last,
                         static_cast<unsigned long long>(f.lengthMismatch), dbc,
                         static_cast<unsigned long long>(f.cyclesMissed),
                         static_cast<unsigned long long>(f.cipChanges));
}

inline int FormatTx(char* out, size_t size, uint64_t guid, const char* span, const TxFigures& f) noexcept {
    char last[32] = "";
    if (f.statusErrors != 0) {
        std::snprintf(last, sizeof(last), " (last 0x%04x)", f.lastBadStatus & 0xFFFFU);
    }
    return std::snprintf(out, size, "[Link] 0x%016llx %s: tx %llu pk, status err %llu%s, cycles missed %llu",
                         static_cast<unsigned long long>(guid), span,
                         static_cast<unsigned long long>(f.packets),
                         static_cast<unsigned long long>(f.statusErrors), last,
                         static_cast<unsigned long long>(f.cyclesMissed));
}

inline int FormatController(char* out, size_t size, const char* span, const ControllerFigures& f) noexcept {
    return std::snprintf(out, size,
                         "[Link] controller %s: cycleLost %llu, cycleInconsistent %llu, cycleTooLong %llu, "
                         "postedWriteErr %llu, regAccessFail %llu, unrecoverableError %llu; "
                         "bus resets %llu (generation %u)",
                         span, static_cast<unsigned long long>(f.cycleLost),
                         static_cast<unsigned long long>(f.cycleInconsistent),
                         static_cast<unsigned long long>(f.cycleTooLong),
                         static_cast<unsigned long long>(f.postedWriteErr),
                         static_cast<unsigned long long>(f.regAccessFail),
                         static_cast<unsigned long long>(f.unrecoverableError),
                         static_cast<unsigned long long>(f.busResets), f.generation);
}

inline int FormatAsync(char* out, size_t size, uint32_t node, const char* span,
                       const AsyncNodeFigures& f) noexcept {
    return std::snprintf(out, size,
                         "[Link] async to node %u %s: %u requests, ack err %u, busy %u, no ack %u, "
                         "timeouts %u, other %u",
                         node, span, f.requests, f.ackErr, f.busy, f.noAck, f.timeouts, f.otherEvent);
}

// One node of the bus as its self-ID packets give it. `ports` holds the
// self-ID port codes: 0 no such port, 1 nothing active on it, 2 towards the
// root, 3 away from it.
inline int FormatNode(char* out, size_t size, uint32_t generation, uint32_t node, bool isLocal, bool isRoot,
                      uint32_t maxSpeedMbps, bool linkActive, uint32_t gapCount, const uint8_t* ports,
                      size_t portCount) noexcept {
    char list[96] = "";
    size_t used = 0;
    for (size_t port = 0; port < portCount && used + 16 < sizeof(list); ++port) {
        const char* state = ports[port] == 3 ? "child" : ports[port] == 2 ? "parent" : "idle";
        if (ports[port] == 0) {
            continue;
        }
        const int n = std::snprintf(list + used, sizeof(list) - used, " %u:%s", static_cast<unsigned>(port), state);
        if (n <= 0) {
            break;
        }
        used += static_cast<size_t>(n);
    }
    const char* role = isLocal ? (isRoot ? " (this controller, root)" : " (this controller)")
                               : (isRoot ? " (root)" : "");
    return std::snprintf(out, size, "[Link] bus generation %u: node %u%s S%u, link %s, gap count %u, ports%s",
                         generation, node, role, maxSpeedMbps, linkActive ? "on" : "off", gapCount,
                         used != 0 ? list : " none");
}

} // namespace ASFW::Diagnostics::Link
