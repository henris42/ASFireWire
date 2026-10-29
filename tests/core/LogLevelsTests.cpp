// Unit tests for the log level (Logging/LogLevels.*): the spec parser and its
// canonical form, the per-category check, the "[Log] level" change line, the
// user-client wire, and that a line which does not log costs no evaluation.

#include "Logging/Logging.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using ASFW::Logging::LogCategory;
using ASFW::Logging::LogLevel;
using ASFW::Logging::LogLevelSettings;
using ASFW::Logging::LogLevelSource;
using ASFW::Logging::LogLevelWire;
using ASFW::Logging::Verbosity;

std::optional<LogLevelSettings> Parse(const char* text) {
    return ASFW::Logging::ParseLogLevelSpec(text, std::strlen(text));
}

std::string Format(const LogLevelSettings& settings) {
    char text[ASFW::Logging::kLogLevelSpecCapacity];
    ASFW::Logging::FormatLogLevelSpec(settings, text, sizeof(text));
    return text;
}

uint8_t OverrideOf(const LogLevelSettings& settings, LogCategory category) {
    return settings.override[static_cast<uint8_t>(category)];
}

constexpr uint8_t Stored(Verbosity verbosity) {
    return static_cast<uint8_t>(verbosity) + 1;
}

// Ring lines after `afterSequence` that contain `needle`.
std::vector<std::string> RingLines(uint64_t afterSequence, const char* needle) {
    ASFW::Logging::LogRingQuery query{};
    query.afterSequence = afterSequence;
    std::strncpy(query.contains, needle, sizeof(query.contains) - 1);
    std::vector<ASFW::Logging::LogRecord> records(64);
    const auto result = ASFW::Logging::LogRing::Shared().Query(
        query, records.data(), static_cast<uint32_t>(records.size()), 1U << 20);
    std::vector<std::string> lines;
    for (uint32_t index = 0; index < result.recordCount; ++index) {
        lines.emplace_back(records[index].message);
    }
    return lines;
}

uint64_t LatestSequence() {
    return ASFW::Logging::LogRing::Shared().Stats().latestSequence;
}

class LogLevelsTests : public ::testing::Test {
protected:
    void SetUp() override {
        ASFW::Logging::LogRing::Shared().Initialize();
        ASFW::Logging::ResetLogLevelForTesting();
    }
    void TearDown() override { ASFW::Logging::ResetLogLevelForTesting(); }

    static void Apply(const char* spec, LogLevelSource source = LogLevelSource::App) {
        const auto settings = Parse(spec);
        ASSERT_TRUE(settings.has_value()) << spec;
        ASFW::Logging::ApplyLogLevel(*settings, source);
    }
};

TEST_F(LogLevelsTests, ParsesAGlobalLevelAndOverrides) {
    const auto plain = Parse("warn");
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->global, Verbosity::Warn);
    EXPECT_EQ(OverrideOf(*plain, LogCategory::IRM), 0);

    const auto mixed = Parse("info irm=debug isoch=trace");
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(mixed->global, Verbosity::Info);
    EXPECT_EQ(OverrideOf(*mixed, LogCategory::IRM), Stored(Verbosity::Debug));
    EXPECT_EQ(OverrideOf(*mixed, LogCategory::Isoch), Stored(Verbosity::Trace));
    EXPECT_EQ(OverrideOf(*mixed, LogCategory::Async), 0);

    // "level=", commas, any case, and "warning" for warn.
    const auto keyed = Parse("level=Info, IRM=DEBUG,directaudio=warning");
    ASSERT_TRUE(keyed.has_value());
    EXPECT_EQ(keyed->global, Verbosity::Info);
    EXPECT_EQ(OverrideOf(*keyed, LogCategory::IRM), Stored(Verbosity::Debug));
    EXPECT_EQ(OverrideOf(*keyed, LogCategory::DirectAudio), Stored(Verbosity::Warn));

    // Overrides alone keep the default global level.
    const auto overridesOnly = Parse("cmp=trace");
    ASSERT_TRUE(overridesOnly.has_value());
    EXPECT_EQ(overridesOnly->global, ASFW::Logging::kDefaultVerbosity);
    EXPECT_EQ(OverrideOf(*overridesOnly, LogCategory::CMP), Stored(Verbosity::Trace));

    const auto off = Parse("off");
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(off->global, Verbosity::Off);
}

TEST_F(LogLevelsTests, RejectsMalformedSpecs) {
    for (const char* bad : {"", "   ", ", ,", "loud", "irm=loud", "nosuch=debug", "warn info",
                            "level=warn debug", "irm=debug irm=trace", "=debug", "irm=",
                            "irm==debug"}) {
        EXPECT_FALSE(Parse(bad).has_value()) << '"' << bad << '"';
    }
    EXPECT_FALSE(ASFW::Logging::ParseLogLevelSpec(nullptr, 4).has_value());
}

TEST_F(LogLevelsTests, ReadsOnlyUpToTheLengthOrTheFirstNul) {
    // OpenCore injects a string device property as data ending in a NUL.
    const char property[] = {'d', 'e', 'b', 'u', 'g', '\0', 'x'};
    const auto fromData = ASFW::Logging::ParseLogLevelSpec(property, sizeof(property));
    ASSERT_TRUE(fromData.has_value());
    EXPECT_EQ(fromData->global, Verbosity::Debug);

    const auto cut = ASFW::Logging::ParseLogLevelSpec("info irm=debug", 4);
    ASSERT_TRUE(cut.has_value());
    EXPECT_EQ(cut->global, Verbosity::Info);
    EXPECT_EQ(OverrideOf(*cut, LogCategory::IRM), 0);
}

TEST_F(LogLevelsTests, FormatsTheCanonicalSpec) {
    EXPECT_EQ(Format(*Parse("WARN")), "warn");
    EXPECT_EQ(Format(*Parse("isoch=trace level=info IRM=debug")), "info irm=debug isoch=trace");

    // Every category overridden still fits the wire.
    std::string all = "error";
    for (const auto& definition : ASFW::Logging::kLogCategoryDefinitions) {
        all += std::string(" ") + definition.name + "=debug";
    }
    const auto settings = Parse(all.c_str());
    ASSERT_TRUE(settings.has_value());
    const std::string formatted = Format(*settings);
    EXPECT_LT(formatted.size(), ASFW::Logging::kLogLevelSpecCapacity);
    EXPECT_EQ(Format(*Parse(formatted.c_str())), formatted);
    EXPECT_NE(formatted.find(" payloadwriter=debug"), std::string::npos);
}

TEST_F(LogLevelsTests, TheLevelGatesEachCategory) {
    // Host tests log everything by default.
    EXPECT_TRUE(ASFW::Logging::Allows(LogCategory::Async, LogLevel::Trace));

    Apply("warn");
    EXPECT_TRUE(ASFW::Logging::Allows(LogCategory::Async, LogLevel::Error));
    EXPECT_TRUE(ASFW::Logging::Allows(LogCategory::Async, LogLevel::Warning));
    EXPECT_FALSE(ASFW::Logging::Allows(LogCategory::Async, LogLevel::Notice));

    Apply("info irm=trace audio=error");
    EXPECT_TRUE(ASFW::Logging::Allows(LogCategory::Async, LogLevel::Notice));
    EXPECT_TRUE(ASFW::Logging::Allows(LogCategory::Async, LogLevel::Info));
    EXPECT_FALSE(ASFW::Logging::Allows(LogCategory::Async, LogLevel::Debug));
    EXPECT_TRUE(ASFW::Logging::Allows(LogCategory::IRM, LogLevel::Trace));
    EXPECT_TRUE(ASFW::Logging::Allows(LogCategory::Audio, LogLevel::Error));
    EXPECT_FALSE(ASFW::Logging::Allows(LogCategory::Audio, LogLevel::Warning));

    Apply("off");
    EXPECT_FALSE(ASFW::Logging::Allows(LogCategory::Controller, LogLevel::Error));
}

TEST_F(LogLevelsTests, ALineThatDoesNotLogIsNeverEvaluated) {
    int evaluations = 0;
    const auto next = [&evaluations] { return ++evaluations; };

    Apply("warn");
    ASFW_LOG(Async, "not logged %d", next());
    ASFW_LOG_DEBUG(Async, "not logged %d", next());
    EXPECT_EQ(evaluations, 0);

    // A line that logs evaluates its arguments once (ring and os_log share the
    // one formatted text).
    const uint64_t before = LatestSequence();
    ASFW_LOG_WARNING(Async, "logged once %d", next());
    EXPECT_EQ(evaluations, 1);
    const auto lines = RingLines(before, "logged once");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "[Async] logged once 1");
}

TEST_F(LogLevelsTests, MilestonesLogFromWarnUpAsNotices) {
    Apply("warn");
    uint64_t before = LatestSequence();
    ASFW_LOG_MILESTONE(Controller, "[StartStep] #%02u %{public}s", 1U, "start");
    ASFW::Logging::LogRingQuery query{};
    query.afterSequence = before;
    std::strncpy(query.contains, "[StartStep] #01", sizeof(query.contains) - 1);
    ASFW::Logging::LogRecord record{};
    ASSERT_EQ(ASFW::Logging::LogRing::Shared().Query(query, &record, 1, 1U << 20).recordCount, 1u);
    EXPECT_STREQ(record.message, "[Controller] [StartStep] #01 start");
    EXPECT_EQ(record.level, static_cast<uint8_t>(LogLevel::Notice));

    Apply("error");
    before = LatestSequence();
    ASFW_LOG_MILESTONE(Controller, "[StartStep] #%02u %{public}s", 2U, "not at error");
    EXPECT_TRUE(RingLines(before, "[StartStep] #02").empty());
}

TEST_F(LogLevelsTests, EachChangeLogsOneLineNamingItsSource) {
    uint64_t before = LatestSequence();
    Apply("warn", LogLevelSource::App);
    auto lines = RingLines(before, "[Log] level");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "[Controller] [Log] level trace -> warn (from app)");
    EXPECT_EQ(ASFW::Logging::CurrentLogLevelSource(), LogLevelSource::App);

    // The same level again is no change: no line, the source stays.
    before = LatestSequence();
    Apply("WARN", LogLevelSource::Agent);
    EXPECT_TRUE(RingLines(before, "[Log] level").empty());
    EXPECT_EQ(ASFW::Logging::CurrentLogLevelSource(), LogLevelSource::App);

    // Turning logging off still says so.
    before = LatestSequence();
    Apply("off", LogLevelSource::CommandLine);
    lines = RingLines(before, "[Log] level");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "[Controller] [Log] level warn -> off (from command line)");

    before = LatestSequence();
    Apply("info irm=debug", LogLevelSource::BootProperty);
    lines = RingLines(before, "[Log] level");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "[Controller] [Log] level off -> info irm=debug (from asfw-log-level property)");
    EXPECT_EQ(Format(ASFW::Logging::CurrentLogLevel()), "info irm=debug");
}

TEST_F(LogLevelsTests, TheWireSetsAndReadsTheLevel) {
    LogLevelWire request{};
    request.source = static_cast<uint32_t>(LogLevelSource::Agent);
    std::strncpy(request.spec, "debug isoch=trace", sizeof(request.spec) - 1);
    ASSERT_TRUE(ASFW::Logging::ApplyLogLevelRequest(&request, sizeof(request)));

    LogLevelWire state{};
    ASFW::Logging::CopyLogLevelState(state);
    EXPECT_STREQ(state.spec, "debug isoch=trace");
    EXPECT_EQ(state.source, static_cast<uint32_t>(LogLevelSource::Agent));

    // A request may stop after its spec's NUL.
    LogLevelWire shortRequest{};
    shortRequest.source = static_cast<uint32_t>(LogLevelSource::App);
    std::strncpy(shortRequest.spec, "info", sizeof(shortRequest.spec) - 1);
    ASSERT_TRUE(ASFW::Logging::ApplyLogLevelRequest(&shortRequest, offsetof(LogLevelWire, spec) + 5));
    ASFW::Logging::CopyLogLevelState(state);
    EXPECT_STREQ(state.spec, "info");

    // Refused without a change: too short, unknown source, bad spec.
    EXPECT_FALSE(ASFW::Logging::ApplyLogLevelRequest(&request, offsetof(LogLevelWire, spec)));
    EXPECT_FALSE(ASFW::Logging::ApplyLogLevelRequest(nullptr, sizeof(request)));
    LogLevelWire badSource = request;
    badSource.source = 99;
    EXPECT_FALSE(ASFW::Logging::ApplyLogLevelRequest(&badSource, sizeof(badSource)));
    LogLevelWire badSpec = request;
    std::strncpy(badSpec.spec, "chatty", sizeof(badSpec.spec) - 1);
    EXPECT_FALSE(ASFW::Logging::ApplyLogLevelRequest(&badSpec, sizeof(badSpec)));
    ASFW::Logging::CopyLogLevelState(state);
    EXPECT_STREQ(state.spec, "info");
}

TEST_F(LogLevelsTests, ALongLineIsCutToTheRecord) {
    const std::string longText(400, 'x');
    const uint64_t before = LatestSequence();
    ASFW_LOG(Audio, "long %{public}s", longText.c_str());
    const auto lines = RingLines(before, "long xxx");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].size(), sizeof(ASFW::Logging::LogRecord{}.message) - 1);
}

} // namespace
