// E-ink controller detection, frame-to-RAM rows, busy levels, command bytes
// (devices/epd/epd_logic.h).

#include <initializer_list>

#include "epd_logic.h"
#include "unity.h"

namespace {

using epd::Controller;

void AssertController(Controller expected, Controller actual) {
    TEST_ASSERT_EQUAL_STRING(epd::Name(expected), epd::Name(actual));
}

void test_epd_detect_ssd_never_answers(void) {
    const uint8_t ff[epd::kVersionSize] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t zero[epd::kVersionSize] = {};
    AssertController(Controller::kSsd1677, epd::Detect(0xFF, ff, 0xFF, ff));
    AssertController(Controller::kSsd1677, epd::Detect(0x00, zero, 0x00, zero));
}

void test_epd_detect_uc8179(void) {
    // A shipping X4 Pro UC8179 (FreeInk XteinkDetect.cpp).
    const uint8_t ver[epd::kVersionSize] = {0x00, 0x00, 0x01, 0xFF, 0xFF};
    AssertController(Controller::kUc8179, epd::Detect(0x13, ver, 0x13, ver));
}

void test_epd_detect_uc8279(void) {
    const uint8_t ver[epd::kVersionSize] = {0x00, 0x03, 0x02, 0x00, 0x00};
    AssertController(Controller::kUc8279, epd::Detect(0x13, ver, 0x13, ver));
}

void test_epd_detect_needs_two_agreeing_passes(void) {
    const uint8_t a[epd::kVersionSize] = {0x00, 0x00, 0x01, 0xFF, 0xFF};
    const uint8_t b[epd::kVersionSize] = {0x00, 0x00, 0x02, 0xFF, 0xFF};
    AssertController(Controller::kSsd1677, epd::Detect(0x13, a, 0x13, b));
    // BUSY_N (FLG bit 0) low: a busy or absent controller is no match.
    AssertController(Controller::kSsd1677, epd::Detect(0x12, a, 0x12, a));
}

void test_epd_screen_type(void) {
    AssertController(Controller::kUc8179, epd::FromScreenType(1));
    AssertController(Controller::kUc8179, epd::FromScreenType(0x0B));
    AssertController(Controller::kUc8279, epd::FromScreenType(2));
    AssertController(Controller::kSsd1677, epd::FromScreenType(3));
}

void test_epd_rows(void) {
    // SSD1677: 480 rows in order.
    TEST_ASSERT_EQUAL_UINT16(480, epd::RamRows(Controller::kSsd1677));
    TEST_ASSERT_EQUAL_INT(0, epd::FrameRowFor(Controller::kSsd1677, 0));
    TEST_ASSERT_EQUAL_INT(479, epd::FrameRowFor(Controller::kSsd1677, 479));
    // UC8179: bottom row first, 120 rows of padding after.
    TEST_ASSERT_EQUAL_UINT16(600, epd::RamRows(Controller::kUc8179));
    TEST_ASSERT_EQUAL_INT(479, epd::FrameRowFor(Controller::kUc8179, 0));
    TEST_ASSERT_EQUAL_INT(0, epd::FrameRowFor(Controller::kUc8179, 479));
    TEST_ASSERT_EQUAL_INT(epd::kPadRow, epd::FrameRowFor(Controller::kUc8179, 480));
    // UC8279: 120 rows of padding first, then in order.
    TEST_ASSERT_EQUAL_INT(epd::kPadRow, epd::FrameRowFor(Controller::kUc8279, 119));
    TEST_ASSERT_EQUAL_INT(0, epd::FrameRowFor(Controller::kUc8279, 120));
    TEST_ASSERT_EQUAL_INT(479, epd::FrameRowFor(Controller::kUc8279, 599));
    // Every frame row goes out exactly once.
    for (const Controller c : {Controller::kSsd1677, Controller::kUc8179, Controller::kUc8279}) {
        int seen[epd::kHeight] = {};
        for (uint16_t i = 0; i < epd::RamRows(c); ++i) {
            const int r = epd::FrameRowFor(c, i);
            if (r != epd::kPadRow) {
                ++seen[r];
            }
        }
        for (const int n : seen) {
            TEST_ASSERT_EQUAL_INT(1, n);
        }
    }
}

void test_epd_busy_polarity(void) {
    TEST_ASSERT_TRUE(epd::Busy(Controller::kSsd1677, 1));
    TEST_ASSERT_FALSE(epd::Busy(Controller::kSsd1677, 0));
    TEST_ASSERT_TRUE(epd::Busy(Controller::kUc8179, 0));
    TEST_ASSERT_FALSE(epd::Busy(Controller::kUc8279, 1));
}

void test_epd_uc8279_window(void) {
    // x 0..799 (end (799 | 7) & 0xFF = 0x1F), gates 120..599.
    const epd::Cmd &w = epd::kUc8279Window;
    TEST_ASSERT_EQUAL_INT(0x90, w.cmd);
    TEST_ASSERT_EQUAL_UINT8(9, w.len);
    TEST_ASSERT_EQUAL_INT(799, (w.data[2] << 8) | w.data[3]);
    TEST_ASSERT_EQUAL_INT(120, (w.data[4] << 8) | w.data[5]);
    TEST_ASSERT_EQUAL_INT(599, (w.data[6] << 8) | w.data[7]);
}

}  // namespace

void RegisterEpdTests(void) {
    RUN_TEST(test_epd_detect_ssd_never_answers);
    RUN_TEST(test_epd_detect_uc8179);
    RUN_TEST(test_epd_detect_uc8279);
    RUN_TEST(test_epd_detect_needs_two_agreeing_passes);
    RUN_TEST(test_epd_screen_type);
    RUN_TEST(test_epd_rows);
    RUN_TEST(test_epd_busy_polarity);
    RUN_TEST(test_epd_uc8279_window);
}
