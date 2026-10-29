//
// LogLevels.hpp
// ASFWDriver
//
// The driver's log level. A line logs (to the LogRing and to os_log) only when
// the level of its category admits the line's severity, and the check comes
// before its arguments are evaluated or formatted: a line that does not log
// costs one relaxed load and a compare, on a branch marked unlikely so the
// logging code sits out of the hot path.
//
// One global level (off, error, warn, info, debug, trace; warn by default) and
// optional per-category overrides, written as a spec: "warn", "info irm=debug",
// "level=info isoch=trace". It is set at run time through the user client
// (selectors 72/73, from the app or asfw-config-agent) and at boot by the
// controller's asfw-log-level property (InitializeLogConfig, LogConfig.hpp).
//

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "LogRing.hpp"

namespace ASFW::Logging {

/// The levels a user picks. Each admits its own severity and every more
/// severe one; info admits Notice (plain ASFW_LOG) and Info lines.
enum class Verbosity : uint8_t {
    Off = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
    Trace = 5,
};

/// One past the least severe LogLevel that `verbosity` admits.
[[nodiscard]] constexpr uint8_t LimitFor(Verbosity verbosity) noexcept {
    switch (verbosity) {
    case Verbosity::Off:
        return 0;
    case Verbosity::Error:
        return static_cast<uint8_t>(LogLevel::Error) + 1;
    case Verbosity::Warn:
        return static_cast<uint8_t>(LogLevel::Warning) + 1;
    case Verbosity::Info:
        return static_cast<uint8_t>(LogLevel::Info) + 1;
    case Verbosity::Debug:
        return static_cast<uint8_t>(LogLevel::Debug) + 1;
    case Verbosity::Trace:
        return static_cast<uint8_t>(LogLevel::Trace) + 1;
    }
    return 0;
}

#ifdef ASFW_HOST_TEST
// Host tests read the lines they check from the ring.
inline constexpr Verbosity kDefaultVerbosity = Verbosity::Trace;
#else
inline constexpr Verbosity kDefaultVerbosity = Verbosity::Warn;
#endif

namespace detail {
inline constexpr uint8_t kDefaultLimit = LimitFor(kDefaultVerbosity);
// Per category, the limit in force XOR kDefaultLimit. Zero-initialized, so
// every category has the default limit from the first instruction (before
// Start, without a constructor), and a check is one load. Hidden: one image
// uses it, so the address is PC-relative rather than loaded from the GOT.
__attribute__((visibility("hidden"))) inline std::atomic<uint8_t>
    gLimitXorDefault[kLogRingCategoryCount]{};
} // namespace detail

[[nodiscard]] inline bool Allows(LogCategory category, LogLevel level) noexcept {
    const auto index = static_cast<uint8_t>(category);
    if (index >= kLogRingCategoryCount) {
        return false;
    }
    const uint8_t limit = static_cast<uint8_t>(
        detail::gLimitXorDefault[index].load(std::memory_order_relaxed) ^ detail::kDefaultLimit);
    return static_cast<uint8_t>(level) < limit;
}

/// Formats an os_log-style line once ("%{public}s" annotations dropped) and
/// writes it to the LogRing and to os_log. The ASFW_LOG macros call it after
/// Allows(); it does not check the level itself.
void EmitLine(LogCategory category, LogLevel level, const char* osLogFormat, ...) noexcept
    __attribute__((format(os_log, 3, 4)));

/// Who set the level; named in the "[Log] level" line and returned by
/// selector 73, so the agent leaves a boot-time level alone.
enum class LogLevelSource : uint32_t {
    Default = 0,
    BootProperty = 1, // the controller's asfw-log-level property
    App = 2,
    Agent = 3,       // asfw-config-agent, re-applying the saved level
    CommandLine = 4, // asfw-config-agent --log-level
};

struct LogLevelSettings {
    Verbosity global{kDefaultVerbosity};
    // Per category: 0 = follows the global level, otherwise Verbosity + 1.
    uint8_t override[kLogRingCategoryCount]{};

    friend bool operator==(const LogLevelSettings&, const LogLevelSettings&) = default;
};

/// Bytes for a spec, NUL included: room for an override on every category.
inline constexpr size_t kLogLevelSpecCapacity = 512;

/// Parses "warn", "info irm=debug", "level=info isoch=trace": a level, then
/// category=level overrides, separated by spaces or commas, case-insensitive.
/// The spec replaces the whole state: a category it leaves out follows the
/// global level, and a spec of overrides alone keeps the default global level.
/// Reads up to `length` bytes or the first NUL. Returns nullopt for an empty
/// spec, an unknown word, or a level or category given twice.
[[nodiscard]] std::optional<LogLevelSettings> ParseLogLevelSpec(const char* text,
                                                                size_t length) noexcept;

/// The canonical spec, "info irm=debug isoch=trace". Returns its length.
size_t FormatLogLevelSpec(const LogLevelSettings& settings, char* out, size_t capacity) noexcept;

[[nodiscard]] const char* VerbosityName(Verbosity verbosity) noexcept;
[[nodiscard]] const char* LogLevelSourceName(LogLevelSource source) noexcept;

[[nodiscard]] LogLevelSettings CurrentLogLevel() noexcept;
[[nodiscard]] LogLevelSource CurrentLogLevelSource() noexcept;

/// Puts `settings` in force. A change logs "[Log] level warn -> debug (from
/// app)", whatever the old and new levels, so the change shows even when the
/// new level is off.
void ApplyLogLevel(const LogLevelSettings& settings, LogLevelSource source) noexcept;

// User-client wire format, selectors 72 (set) and 73 (get). The set request
// carries who asks and the spec; both selectors answer with the level in force
// and who set it.
struct __attribute__((packed)) LogLevelWire {
    uint32_t source{0}; // LogLevelSource
    uint32_t reserved{0};
    char spec[kLogLevelSpecCapacity]{}; // NUL-terminated
};
static_assert(sizeof(LogLevelWire) == 8 + kLogLevelSpecCapacity,
              "log level wire layout is frozen");

/// Selector 72: parses and applies a request of `length` bytes. False when it
/// is too short, names an unknown source, or carries an invalid spec; the level
/// is then unchanged.
[[nodiscard]] bool ApplyLogLevelRequest(const void* bytes, size_t length) noexcept;

/// Selectors 72 and 73: the level in force.
void CopyLogLevelState(LogLevelWire& out) noexcept;

#ifdef ASFW_HOST_TEST
/// Back to the default level and source, without a "[Log]" line.
void ResetLogLevelForTesting() noexcept;
#endif

} // namespace ASFW::Logging
