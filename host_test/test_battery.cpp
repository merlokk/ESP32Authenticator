// CW2017 fuel gauge decisions (devices/battery/cw2017_logic.h).

#include <cstring>

#include "cw2017_logic.h"
#include "unity.h"

namespace {

namespace cw = battery::cw2017;

void AssertAction(cw::Action expected, cw::Action actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

void test_vcell_formula(void) {
    TEST_ASSERT_EQUAL_UINT16(0, cw::VcellMillivolts(0x00, 0x00));
    // 312.5 uV per LSB, rounded: 12800 -> 4000 mV, 1 -> 0 mV, 2 -> 1 mV.
    TEST_ASSERT_EQUAL_UINT16(4000, cw::VcellMillivolts(0x32, 0x00));
    TEST_ASSERT_EQUAL_UINT16(0, cw::VcellMillivolts(0x00, 0x01));
    TEST_ASSERT_EQUAL_UINT16(1, cw::VcellMillivolts(0x00, 0x02));
    // 14 bits: the two top bits of the high byte are not voltage.
    TEST_ASSERT_EQUAL_UINT16(cw::VcellMillivolts(0x32, 0x10), cw::VcellMillivolts(0xF2, 0x10));
    TEST_ASSERT_EQUAL_UINT16(5120, cw::VcellMillivolts(0x3F, 0xFF));
}

void test_version_running(void) {
    TEST_ASSERT_TRUE(cw::VersionRunning(0x0D));
    TEST_ASSERT_TRUE(cw::VersionRunning(0x0F));
    TEST_ASSERT_FALSE(cw::VersionRunning(0xA0));  // starting
    TEST_ASSERT_FALSE(cw::VersionRunning(0x00));
    TEST_ASSERT_FALSE(cw::VersionRunning(0xFF));
}

void test_profile_matches_needs_flag_and_bytes(void) {
    uint8_t resident[cw::kBatinfoSize];
    memcpy(resident, cw::kBatinfo, sizeof resident);
    TEST_ASSERT_TRUE(cw::ProfileMatches(cw::kUpdateFlag, resident));
    TEST_ASSERT_TRUE(cw::ProfileMatches(cw::kUpdateFlag | 0x05, resident));  // alert level kept
    TEST_ASSERT_FALSE(cw::ProfileMatches(0x00, resident));
    resident[cw::kBatinfoSize - 1] ^= 1;
    TEST_ASSERT_FALSE(cw::ProfileMatches(cw::kUpdateFlag, resident));
}

void test_profile_is_the_stock_table(void) {
    TEST_ASSERT_EQUAL_UINT8(0x50, cw::kBatinfo[0]);
    TEST_ASSERT_EQUAL_UINT8(0x23, cw::kBatinfo[cw::kBatinfoSize - 1]);
    TEST_ASSERT_EQUAL_UINT(0x60, cw::kRegBatinfo + cw::kBatinfoSize);  // ends at 0x5F
}

void test_decide(void) {
    // Running with our profile: read only.
    AssertAction(cw::Action::kNone, cw::Decide(cw::kModeNormal, 0x0D, 80, true));
    // Ours, but asleep, starting or without a valid SoC: soft reset.
    AssertAction(cw::Action::kRestart, cw::Decide(cw::kModeDefault, 0x0D, 80, true));
    AssertAction(cw::Action::kRestart, cw::Decide(cw::kModeNormal, 0xA0, 80, true));
    AssertAction(cw::Action::kRestart, cw::Decide(cw::kModeNormal, 0x0D, 101, true));
    // Another or no profile: upload, whatever the rest says.
    AssertAction(cw::Action::kUpload, cw::Decide(cw::kModeNormal, 0x0D, 80, false));
    AssertAction(cw::Action::kUpload, cw::Decide(cw::kModeDefault, 0xA0, 0, false));
}

}  // namespace

void RegisterBatteryTests(void) {
    RUN_TEST(test_vcell_formula);
    RUN_TEST(test_version_running);
    RUN_TEST(test_profile_matches_needs_flag_and_bytes);
    RUN_TEST(test_profile_is_the_stock_table);
    RUN_TEST(test_decide);
}
