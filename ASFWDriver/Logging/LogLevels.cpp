//
// LogLevels.cpp
// ASFWDriver
//

#include "LogLevels.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <os/log.h>

namespace ASFW::Logging {

namespace {

constexpr Verbosity kVerbosities[] = {Verbosity::Off,  Verbosity::Error, Verbosity::Warn,
                                      Verbosity::Info, Verbosity::Debug, Verbosity::Trace};

// Room for the longest canonical spec: the global level and an override on
// every category ("<name>=<level>", each level at most five letters).
constexpr size_t LongestSpec() noexcept {
    size_t length = 5;
    for (const auto& definition : kLogCategoryDefinitions) {
        length += 1 + CatalogNameLength(definition.name) + 1 + 5;
    }
    return length;
}
static_assert(LongestSpec() < kLogLevelSpecCapacity, "a full spec must fit the wire");

std::atomic<uint32_t> gSource{static_cast<uint32_t>(LogLevelSource::Default)};
// The settings in force, for CurrentLogLevel; the per-category limits the
// checks read are derived from them (detail::gLimitXorDefault). Both change
// only under gApplyLock (the app and the agent may set the level at once).
LogLevelSettings gSettings{};
std::atomic_flag gApplyLock = ATOMIC_FLAG_INIT;

void LockApply() noexcept {
    while (gApplyLock.test_and_set(std::memory_order_acquire)) {
    }
}

void UnlockApply() noexcept {
    gApplyLock.clear(std::memory_order_release);
}

char Lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoringCase(const char* text, size_t length, const char* word) noexcept {
    size_t index = 0;
    for (; index < length; ++index) {
        if (word[index] == '\0' || Lower(text[index]) != Lower(word[index])) {
            return false;
        }
    }
    return word[index] == '\0';
}

std::optional<Verbosity> ParseVerbosity(const char* text, size_t length) noexcept {
    for (const Verbosity verbosity : kVerbosities) {
        if (EqualsIgnoringCase(text, length, VerbosityName(verbosity))) {
            return verbosity;
        }
    }
    if (EqualsIgnoringCase(text, length, "warning")) {
        return Verbosity::Warn;
    }
    return std::nullopt;
}

std::optional<uint8_t> ParseCategory(const char* text, size_t length) noexcept {
    for (const auto& definition : kLogCategoryDefinitions) {
        if (EqualsIgnoringCase(text, length, definition.name)) {
            return static_cast<uint8_t>(definition.category);
        }
    }
    return std::nullopt;
}

bool IsSeparator(char c) noexcept {
    return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r';
}

void Append(char* out, size_t capacity, size_t& used, const char* text) noexcept {
    for (size_t index = 0; text[index] != '\0' && used + 1 < capacity; ++index) {
        out[used++] = text[index];
    }
    out[used] = '\0';
}

} // namespace

void EmitLine(LogCategory category, LogLevel level, const char* osLogFormat, ...) noexcept {
    if (osLogFormat == nullptr) {
        return;
    }
    char format[512];
    SanitizeOsLogFormat(osLogFormat, format, sizeof(format));
    // The unified log keeps about 240 characters of a dext's line.
    char text[256];
    va_list args;
    va_start(args, osLogFormat);
    (void)vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    LogRing::Shared().AppendText(category, level, text);
    os_log(OS_LOG_DEFAULT, "%{public}s", text);
}

const char* VerbosityName(Verbosity verbosity) noexcept {
    switch (verbosity) {
    case Verbosity::Off:
        return "off";
    case Verbosity::Error:
        return "error";
    case Verbosity::Warn:
        return "warn";
    case Verbosity::Info:
        return "info";
    case Verbosity::Debug:
        return "debug";
    case Verbosity::Trace:
        return "trace";
    }
    return "?";
}

const char* LogLevelSourceName(LogLevelSource source) noexcept {
    switch (source) {
    case LogLevelSource::Default:
        return "default";
    case LogLevelSource::BootProperty:
        return "asfw-log-level property";
    case LogLevelSource::App:
        return "app";
    case LogLevelSource::Agent:
        return "agent";
    case LogLevelSource::CommandLine:
        return "command line";
    }
    return "?";
}

std::optional<LogLevelSettings> ParseLogLevelSpec(const char* text, size_t length) noexcept {
    if (text == nullptr) {
        return std::nullopt;
    }
    size_t end = 0;
    while (end < length && text[end] != '\0') {
        ++end;
    }

    LogLevelSettings settings{};
    bool haveGlobal = false;
    bool haveAny = false;
    size_t index = 0;
    while (index < end) {
        while (index < end && IsSeparator(text[index])) {
            ++index;
        }
        if (index == end) {
            break;
        }
        const size_t start = index;
        while (index < end && !IsSeparator(text[index])) {
            ++index;
        }
        const char* token = text + start;
        const size_t tokenLength = index - start;
        const auto* equals = static_cast<const char*>(memchr(token, '=', tokenLength));

        const char* key = token;
        size_t keyLength = 0;
        const char* value = token;
        size_t valueLength = tokenLength;
        if (equals != nullptr) {
            keyLength = static_cast<size_t>(equals - token);
            value = equals + 1;
            valueLength = tokenLength - keyLength - 1;
        }
        const auto verbosity = ParseVerbosity(value, valueLength);
        if (!verbosity) {
            return std::nullopt;
        }
        if (keyLength == 0 && equals != nullptr) {
            return std::nullopt; // "=debug"
        }
        if (equals == nullptr || EqualsIgnoringCase(key, keyLength, "level")) {
            if (haveGlobal) {
                return std::nullopt;
            }
            haveGlobal = true;
            settings.global = *verbosity;
        } else {
            const auto category = ParseCategory(key, keyLength);
            if (!category || settings.override[*category] != 0) {
                return std::nullopt;
            }
            settings.override[*category] = static_cast<uint8_t>(*verbosity) + 1;
        }
        haveAny = true;
    }
    if (!haveAny) {
        return std::nullopt;
    }
    return settings;
}

size_t FormatLogLevelSpec(const LogLevelSettings& settings, char* out, size_t capacity) noexcept {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    size_t used = 0;
    out[0] = '\0';
    Append(out, capacity, used, VerbosityName(settings.global));
    for (const auto& definition : kLogCategoryDefinitions) {
        const uint8_t override = settings.override[static_cast<uint8_t>(definition.category)];
        if (override == 0) {
            continue;
        }
        char name[kLogCategoryNameCapacity + 1]{};
        for (size_t index = 0; index < kLogCategoryNameCapacity && definition.name[index] != '\0';
             ++index) {
            name[index] = Lower(definition.name[index]);
        }
        Append(out, capacity, used, " ");
        Append(out, capacity, used, name);
        Append(out, capacity, used, "=");
        Append(out, capacity, used, VerbosityName(static_cast<Verbosity>(override - 1)));
    }
    return used;
}

LogLevelSettings CurrentLogLevel() noexcept {
    LockApply();
    const LogLevelSettings settings = gSettings;
    UnlockApply();
    return settings;
}

LogLevelSource CurrentLogLevelSource() noexcept {
    return static_cast<LogLevelSource>(gSource.load(std::memory_order_relaxed));
}

void ApplyLogLevel(const LogLevelSettings& settings, LogLevelSource source) noexcept {
    LockApply();
    const LogLevelSettings previous = gSettings;
    gSettings = settings;
    for (uint32_t index = 0; index < kLogRingCategoryCount; ++index) {
        const uint8_t override = settings.override[index];
        const Verbosity verbosity =
            override == 0 ? settings.global : static_cast<Verbosity>(override - 1);
        detail::gLimitXorDefault[index].store(
            static_cast<uint8_t>(LimitFor(verbosity) ^ detail::kDefaultLimit),
            std::memory_order_relaxed);
    }
    const bool changed = !(previous == settings);
    if (changed) {
        gSource.store(static_cast<uint32_t>(source), std::memory_order_relaxed);
    }
    UnlockApply();

    if (changed) {
        char from[kLogLevelSpecCapacity];
        char to[kLogLevelSpecCapacity];
        (void)FormatLogLevelSpec(previous, from, sizeof(from));
        (void)FormatLogLevelSpec(settings, to, sizeof(to));
        EmitLine(LogCategory::Controller, LogLevel::Notice,
                 "[Controller] [Log] level %{public}s -> %{public}s (from %{public}s)", from, to,
                 LogLevelSourceName(source));
    }
}

bool ApplyLogLevelRequest(const void* bytes, size_t length) noexcept {
    constexpr size_t kHeader = offsetof(LogLevelWire, spec);
    if (bytes == nullptr || length <= kHeader) {
        return false;
    }
    LogLevelWire request{};
    memcpy(&request, bytes, length < sizeof(request) ? length : sizeof(request));
    if (request.source > static_cast<uint32_t>(LogLevelSource::CommandLine)) {
        return false;
    }
    const size_t specLength = (length < sizeof(request) ? length : sizeof(request)) - kHeader;
    const auto settings = ParseLogLevelSpec(request.spec, specLength);
    if (!settings) {
        return false;
    }
    ApplyLogLevel(*settings, static_cast<LogLevelSource>(request.source));
    return true;
}

void CopyLogLevelState(LogLevelWire& out) noexcept {
    out = LogLevelWire{};
    out.source = static_cast<uint32_t>(CurrentLogLevelSource());
    (void)FormatLogLevelSpec(CurrentLogLevel(), out.spec, sizeof(out.spec));
}

#ifdef ASFW_HOST_TEST
void ResetLogLevelForTesting() noexcept {
    LockApply();
    gSettings = LogLevelSettings{};
    for (auto& limit : detail::gLimitXorDefault) {
        limit.store(0, std::memory_order_relaxed);
    }
    gSource.store(static_cast<uint32_t>(LogLevelSource::Default), std::memory_order_relaxed);
    UnlockApply();
}
#endif

} // namespace ASFW::Logging
