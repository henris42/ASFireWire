#include <gtest/gtest.h>

#include "Audio/DriverKit/UnitSerialLabel.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace {

using ASFW::DeviceProfiles::Audio::kMotuVendorId;
using ASFW::Isoch::Audio::FormatUnitSerial;

TEST(UnitSerialLabelTests, MotuSerialIsTheDecimalGuidExtension) {
    char out[24] = {};
    EXPECT_EQ(FormatUnitSerial(kMotuVendorId, 0x0001f2000001f717ULL, out, sizeof(out)), 6u);
    EXPECT_STREQ(out, "128791");
    EXPECT_EQ(FormatUnitSerial(kMotuVendorId, 0x0001f2000000f944ULL, out, sizeof(out)), 5u);
    EXPECT_STREQ(out, "63812");
}

TEST(UnitSerialLabelTests, OtherVendorsGetTheHexExtension) {
    char out[24] = {};
    EXPECT_EQ(FormatUnitSerial(0x00130e, 0x00130e01000a2b3cULL, out, sizeof(out)), 10u);
    EXPECT_STREQ(out, "01000A2B3C");
}

TEST(UnitSerialLabelTests, ZeroGuidAndSmallBuffersYieldNothing) {
    char out[24] = {'x'};
    EXPECT_EQ(FormatUnitSerial(kMotuVendorId, 0, out, sizeof(out)), 0u);
    EXPECT_STREQ(out, "");
    char tiny[4] = {};
    EXPECT_EQ(FormatUnitSerial(kMotuVendorId, 0x0001f2000001f717ULL, tiny, sizeof(tiny)), 0u);
    EXPECT_STREQ(tiny, "");
    EXPECT_EQ(FormatUnitSerial(kMotuVendorId, 0x0001f2000001f717ULL, nullptr, 0), 0u);
}

} // namespace
