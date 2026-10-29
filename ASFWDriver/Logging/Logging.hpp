#pragma once

#include <os/log.h>
#include <atomic>
#ifdef ASFW_HOST_TEST
#include <mach/mach_time.h>
#else
#include <DriverKit/IOLib.h> // mach_time
#endif

#include "LogConfig.hpp"
#include "IOReturnName.hpp"
#include "LogLevels.hpp"
#include "LogRing.hpp"

#ifndef OS_LOG_TYPE_DEFAULT
#define OS_LOG_TYPE_DEFAULT static_cast<os_log_type_t>(0x00)
#endif

#ifndef OS_LOG_TYPE_INFO
#define OS_LOG_TYPE_INFO static_cast<os_log_type_t>(0x01)
#endif

#ifndef OS_LOG_TYPE_ERROR
#define OS_LOG_TYPE_ERROR static_cast<os_log_type_t>(0x10)
#endif

#ifndef OS_LOG_TYPE_FAULT
#define OS_LOG_TYPE_FAULT static_cast<os_log_type_t>(0x11)
#endif

#ifndef OS_LOG_TYPE_DEBUG
#define OS_LOG_TYPE_DEBUG OS_LOG_TYPE_DEFAULT
#endif

#ifndef os_log_info
#define os_log_info(log, fmt, ...) os_log_with_type((log), OS_LOG_TYPE_INFO, fmt, ##__VA_ARGS__)
#endif

#ifndef os_log_error
#define os_log_error(log, fmt, ...) os_log_with_type((log), OS_LOG_TYPE_ERROR, fmt, ##__VA_ARGS__)
#endif

#ifndef os_log_debug
#define os_log_debug(log, fmt, ...) os_log_with_type((log), OS_LOG_TYPE_DEBUG, fmt, ##__VA_ARGS__)
#endif

#ifndef os_log_warning
#define os_log_warning(log, fmt, ...) os_log_with_type((log), OS_LOG_TYPE_DEFAULT, fmt, ##__VA_ARGS__)
#endif

#ifndef os_log_with_type
#define os_log_with_type(log, type, fmt, ...) os_log((log), fmt, ##__VA_ARGS__)
#endif

#ifndef ASFW_DEBUG_BUS_RESET_PACKET
#define ASFW_DEBUG_BUS_RESET_PACKET 0
#endif

#ifndef ASFW_DEBUG_TEMP_RX_TX
#define ASFW_DEBUG_TEMP_RX_TX 0
#endif

#ifndef ASFW_DEBUG_CONFIG_ROM
#define ASFW_DEBUG_CONFIG_ROM 0
#endif

#ifndef ASFW_DEBUG_PHY_INIT
#define ASFW_DEBUG_PHY_INIT 1
#endif

#ifndef ASFW_DEBUG_SELF_ID
#define ASFW_DEBUG_SELF_ID 1
#endif

#ifndef ASFW_DEBUG_TOPOLOGY
#define ASFW_DEBUG_TOPOLOGY 1
#endif

#ifndef ASFW_DEBUG_BUS_RESET
#define ASFW_DEBUG_BUS_RESET 0
#endif

//
// Keep compatibility with your existing Logging.cpp (ASFW::Driver::Logging:*)
// and add header-only rate-limited macros + stable category prefixes.
//

namespace ASFW::Driver::Logging {
os_log_t Controller();  // defined in Logging.cpp you shared
os_log_t Hardware();
os_log_t BusReset();
os_log_t Topology();
os_log_t Metrics();
os_log_t Async();
os_log_t UserClient();
os_log_t Discovery();
os_log_t IRM();
os_log_t BusManager();
os_log_t ConfigROM();
os_log_t MusicSubunit();
os_log_t FCP();
os_log_t CMP();
os_log_t AVC();
os_log_t Isoch();
os_log_t Audio();
os_log_t DirectAudio();
os_log_t DICE();
os_log_t Zts();
os_log_t TxSyt();
os_log_t PayloadWriter();
os_log_t Oxfw();
} // namespace ASFW::Driver::Logging

// ----- time helpers (header-only, safe in DriverKit) -----
namespace ASFW::LogDetail {
inline uint64_t NowNs() {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    const uint64_t t = mach_absolute_time();
    return (t * tb.numer) / tb.denom;
}
struct RlState {
    std::atomic<uint64_t> last_ns{0};
    std::atomic<uint64_t> suppressed{0};
};
} // namespace ASFW::LogDetail

// ----- Leveled logging -----
//
// Every line has a severity and logs only when the log level of its category
// admits it (LogLevels.hpp). The check comes first, so the arguments of a line
// that does not log are neither evaluated nor formatted. A line that logs is
// formatted once and goes to the driver's LogRing (queryable through the user
// client) and to os_log.
//
//   ASFW_LOG_ERROR    a failure: the driver, a device or a stream stops working
//   ASFW_LOG_WARNING  a refusal or an anomaly the user should see
//   ASFW_LOG, _INFO   a state change: start, stop, device found, rate set
//   ASFW_LOG_DEBUG    detail, and heartbeats (at most about one per second)
//   ASFW_LOG_TRACE    per-packet and per-transaction lines, dumps
//
// The default level, warn, logs errors, warnings and milestones only.
#define ASFW_LOG_ENABLED(cat, level)                                              \
    ::ASFW::Logging::Allows(::ASFW::Logging::LogCategory::cat,                    \
                            ::ASFW::Logging::LogLevel::level)

// The macros' own test: logging is the rare path, so the compiler moves the
// call and its argument set-up out of line.
#define ASFW_LOG_LIKELY_OFF(cat, level) __builtin_expect(ASFW_LOG_ENABLED(cat, level), 0)

// The pragmas make a format mismatch an error, as os_log itself does.
#define ASFW_LOG_AT(cat, level, fmt, ...)                                         \
    do {                                                                          \
        if (ASFW_LOG_LIKELY_OFF(cat, level)) {                                    \
            _Pragma("clang diagnostic push")                                      \
            _Pragma("clang diagnostic error \"-Wformat\"")                        \
            ::ASFW::Logging::EmitLine(::ASFW::Logging::LogCategory::cat,          \
                                      ::ASFW::Logging::LogLevel::level,           \
                                      "[" #cat "] " fmt, ##__VA_ARGS__);          \
            _Pragma("clang diagnostic pop")                                       \
        }                                                                         \
    } while (0)

#define ASFW_LOG_ERROR(cat, fmt, ...)   ASFW_LOG_AT(cat, Error, fmt, ##__VA_ARGS__)
#define ASFW_LOG_FAULT(cat, fmt, ...)   ASFW_LOG_AT(cat, Error, fmt, ##__VA_ARGS__)
#define ASFW_LOG_WARNING(cat, fmt, ...) ASFW_LOG_AT(cat, Warning, fmt, ##__VA_ARGS__)
#define ASFW_LOG(cat, fmt, ...)         ASFW_LOG_AT(cat, Notice, fmt, ##__VA_ARGS__)
#define ASFW_LOG_INFO(cat, fmt, ...)    ASFW_LOG_AT(cat, Info, fmt, ##__VA_ARGS__)
#define ASFW_LOG_DEBUG(cat, fmt, ...)   ASFW_LOG_AT(cat, Debug, fmt, ##__VA_ARGS__)
#define ASFW_LOG_TRACE(cat, fmt, ...)   ASFW_LOG_AT(cat, Trace, fmt, ##__VA_ARGS__)

// A line that every level from warn up records, so a default log still shows
// it: the numbered bring-up steps and the log level in force. It is stored as
// a notice, not a warning.
#define ASFW_LOG_MILESTONE(cat, fmt, ...)                                         \
    do {                                                                          \
        if (ASFW_LOG_LIKELY_OFF(cat, Warning)) {                                  \
            _Pragma("clang diagnostic push")                                      \
            _Pragma("clang diagnostic error \"-Wformat\"")                        \
            ::ASFW::Logging::EmitLine(::ASFW::Logging::LogCategory::cat,          \
                                      ::ASFW::Logging::LogLevel::Notice,          \
                                      "[" #cat "] " fmt, ##__VA_ARGS__);          \
            _Pragma("clang diagnostic pop")                                       \
        }                                                                         \
    } while (0)

// Ring only: never reaches os_log. For real-time paths.
#define ASFW_LOG_RING_ONLY(cat, level, fmt, ...)                                  \
    do {                                                                          \
        if (ASFW_LOG_LIKELY_OFF(cat, level)) {                                    \
            ::ASFW::Logging::RingLog(::ASFW::Logging::LogCategory::cat,           \
                                     ::ASFW::Logging::LogLevel::level,            \
                                     "[" #cat "] " fmt, ##__VA_ARGS__);           \
        }                                                                         \
    } while (0)

// ----- Rate-limited logging -----
// key: per-callsite stable string (e.g. "tx/ack_tardy"); interval_ms: throttle
// window; level: a LogLevel name (Error, Warning, Notice, Info, Debug, Trace).
#define ASFW_LOG_RL(cat, key, interval_ms, level, fmt, ...)                                    \
    do {                                                                                        \
        if (ASFW_LOG_LIKELY_OFF(cat, level)) {                                                  \
            static ASFW::LogDetail::RlState _s;                                                 \
            const uint64_t _now = ASFW::LogDetail::NowNs();                                     \
            const uint64_t _intv = (uint64_t)(interval_ms) * 1000000ull;                        \
            const uint64_t _last = _s.last_ns.load(std::memory_order_relaxed);                  \
            if (_now - _last >= _intv || _last == 0) {                                          \
                if (_s.last_ns.exchange(_now, std::memory_order_relaxed) != 0) {                \
                    const uint64_t _lost = _s.suppressed.exchange(0, std::memory_order_relaxed);\
                    if (_lost) {                                                                \
                        ::ASFW::Logging::EmitLine(::ASFW::Logging::LogCategory::cat,            \
                            ::ASFW::Logging::LogLevel::level,                                   \
                            "[" #cat "][%{public}s] (suppressed=%llu prior)", key, _lost);      \
                    }                                                                           \
                }                                                                               \
                _Pragma("clang diagnostic push")                                                \
                _Pragma("clang diagnostic error \"-Wformat\"")                                  \
                ::ASFW::Logging::EmitLine(::ASFW::Logging::LogCategory::cat,                    \
                    ::ASFW::Logging::LogLevel::level, "[" #cat "][%{public}s] " fmt, key,       \
                    ##__VA_ARGS__);                                                             \
                _Pragma("clang diagnostic pop")                                                 \
            } else {                                                                            \
                _s.suppressed.fetch_add(1, std::memory_order_relaxed);                          \
            }                                                                                   \
        }                                                                                       \
    } while (0)

// Ring-only variant for a persistent anomaly on a real-time path.  It keeps
// enough chronology for MCP while never trapping out to the unified-log path.
#define ASFW_LOG_RING_ONLY_RL(cat, key, interval_ms, level, fmt, ...)                          \
    do {                                                                                        \
        if (ASFW_LOG_LIKELY_OFF(cat, level)) {                                                  \
            static ASFW::LogDetail::RlState _s;                                                 \
            const uint64_t _now = ASFW::LogDetail::NowNs();                                     \
            const uint64_t _intv = (uint64_t)(interval_ms) * 1000000ull;                        \
            const uint64_t _last = _s.last_ns.load(std::memory_order_relaxed);                  \
            if (_now - _last >= _intv || _last == 0) {                                          \
                if (_s.last_ns.exchange(_now, std::memory_order_relaxed) != 0) {                \
                    const uint64_t _lost = _s.suppressed.exchange(0, std::memory_order_relaxed);\
                    if (_lost) {                                                                \
                        ::ASFW::Logging::RingLog(::ASFW::Logging::LogCategory::cat,             \
                            ::ASFW::Logging::LogLevel::level,                                   \
                            "[" #cat "][%s] (suppressed=%llu prior)", key, _lost);              \
                    }                                                                           \
                }                                                                               \
                ::ASFW::Logging::RingLog(::ASFW::Logging::LogCategory::cat,                     \
                    ::ASFW::Logging::LogLevel::level, "[" #cat "][%s] " fmt, key,               \
                    ##__VA_ARGS__);                                                             \
            } else {                                                                            \
                _s.suppressed.fetch_add(1, std::memory_order_relaxed);                          \
            }                                                                                   \
        }                                                                                       \
    } while (0)

// ----- Site-aware structured logging (for AT state correlation) -----
// Adds source file/line/function for debugging
#ifndef __FILE_NAME__
#define __FILE_NAME__ __FILE__
#endif

#define ASFW_LOG_SITE(cat, fmt, ...)                                                     \
    ASFW_LOG(cat, "%{public}s:%d %{public}s | " fmt, __FILE_NAME__, __LINE__, __func__,  \
             ##__VA_ARGS__)

// ----- Correlated logging with txid/gen (parseable k=v format) -----
#define ASFW_LOG_KV(cat, ctxName, txid, gen, fmt, ...) \
    ASFW_LOG_SITE(cat, "ctx=%{public}s txid=%u gen=%u " fmt, ctxName, (unsigned)(txid), (unsigned)(gen), ##__VA_ARGS__)

#if ASFW_DEBUG_SELF_ID
#define ASFW_LOG_SELF_ID(fmt, ...) ASFW_LOG_DEBUG(Hardware, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_SELF_ID(fmt, ...)
#endif

#if ASFW_DEBUG_TOPOLOGY
#define ASFW_LOG_TOPOLOGY_DETAIL(fmt, ...) ASFW_LOG_DEBUG(Topology, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_TOPOLOGY_DETAIL(fmt, ...)
#endif

#if ASFW_DEBUG_BUS_RESET
#define ASFW_LOG_BUSRESET_DETAIL(fmt, ...) ASFW_LOG_DEBUG(BusReset, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_BUSRESET_DETAIL(fmt, ...)
#endif

#if ASFW_DEBUG_BUS_RESET_PACKET
#define ASFW_LOG_BUS_RESET_PACKET(fmt, ...) ASFW_LOG_DEBUG(Async, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_BUS_RESET_PACKET(fmt, ...)
#endif

#if ASFW_DEBUG_CONFIG_ROM
#define ASFW_LOG_CONFIG_ROM(fmt, ...) ASFW_LOG_DEBUG(ConfigROM, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_CONFIG_ROM(fmt, ...)
#endif

#if ASFW_DEBUG_PHY_INIT
#define ASFW_LOG_PHY(fmt, ...) ASFW_LOG_DEBUG(Hardware, fmt, ##__VA_ARGS__)
#else
#define ASFW_LOG_PHY(fmt, ...)
#endif

// ----- Verbosity-numbered logging -----
//
// The older numbered macros, mapped onto the severities above: V0 failures
// and refusals (warning), V1 compact summaries (notice), V2 transitions
// (debug), V3/V4 detailed flow and dumps (trace). Hex dumps are trace.
#define ASFW_LOG_V0(category, fmt, ...) ASFW_LOG_AT(category, Warning, fmt, ##__VA_ARGS__)
#define ASFW_LOG_V1(category, fmt, ...) ASFW_LOG_AT(category, Notice, fmt, ##__VA_ARGS__)
#define ASFW_LOG_V2(category, fmt, ...) ASFW_LOG_AT(category, Debug, fmt, ##__VA_ARGS__)
#define ASFW_LOG_V3(category, fmt, ...) ASFW_LOG_AT(category, Trace, fmt, ##__VA_ARGS__)
#define ASFW_LOG_V4(category, fmt, ...) ASFW_LOG_AT(category, Trace, fmt, ##__VA_ARGS__)
#define ASFW_LOG_HEX(category, fmt, ...) ASFW_LOG_AT(category, Trace, fmt, ##__VA_ARGS__)
