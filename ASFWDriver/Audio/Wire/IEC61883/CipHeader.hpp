#pragma once

#include <cstdint>

namespace ASFW::Protocols::Audio::IEC61883 {

struct CipHeaderWords final {
    uint32_t q0{0};
    uint32_t q1{0};
};

struct CipHeaderConfig final {
    uint8_t sid{0};
    uint8_t dbs{0};
    uint8_t fn{0};
    uint8_t qpc{0};
    bool sph{false};
    uint8_t fmt{0x10};
    uint8_t fdf{0x02};
    uint8_t noDataFdf{0xFF};
};

class CipHeaderBuilder final {
public:
    CipHeaderBuilder() noexcept = default;
    explicit CipHeaderBuilder(const CipHeaderConfig& config) noexcept;

    void Configure(const CipHeaderConfig& config) noexcept;
    // Our node id changes with every bus reset; the configuration keeps the rest.
    void SetSid(uint8_t sid) noexcept { config_.sid = static_cast<uint8_t>(sid & 0x3F); }
    [[nodiscard]] uint8_t Sid() const noexcept { return config_.sid; }
    [[nodiscard]] const CipHeaderConfig& Config() const noexcept;

    [[nodiscard]] CipHeaderWords BuildData(uint8_t dbc, uint16_t syt) const noexcept;
    [[nodiscard]] CipHeaderWords BuildNoData(uint8_t dbc) const noexcept;

private:
    CipHeaderConfig config_{};
};

} // namespace ASFW::Protocols::Audio::IEC61883
