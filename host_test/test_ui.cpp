// The portrait screen on the landscape panel, the full-refresh plan
// (components/ui/ui_logic.h).

#include <cstring>
#include <vector>

#include "ui_logic.h"
#include "unity.h"

namespace {

bool White(const std::vector<uint8_t> &buf, size_t row_bytes, int x, int y) {
    return (buf[y * row_bytes + x / 8] & (0x80 >> (x % 8))) != 0;
}

void SetBlack(std::vector<uint8_t> &buf, size_t row_bytes, int x, int y) {
    buf[y * row_bytes + x / 8] = static_cast<uint8_t>(buf[y * row_bytes + x / 8] & ~(0x80 >> (x % 8)));
}

void test_ui_sizes(void) {
    TEST_ASSERT_EQUAL_UINT16(480, ui::kWidth);
    TEST_ASSERT_EQUAL_UINT16(800, ui::kHeight);
    TEST_ASSERT_EQUAL_size_t(epd::kFrameBytes, ui::kScreenBytes);
    // The portrait top is the frame's left (held as a book).
    TEST_ASSERT_EQUAL_UINT8(epd::kInsetLeft, ui::kInsetTop);
}

void test_ui_corners(void) {
    // Portrait top left is frame bottom left; portrait top right, frame top left.
    TEST_ASSERT_EQUAL_UINT16(0, ui::PanelX(0, 0));
    TEST_ASSERT_EQUAL_UINT16(479, ui::PanelY(0, 0));
    TEST_ASSERT_EQUAL_UINT16(0, ui::PanelX(479, 0));
    TEST_ASSERT_EQUAL_UINT16(0, ui::PanelY(479, 0));
    TEST_ASSERT_EQUAL_UINT16(799, ui::PanelX(0, 799));
    TEST_ASSERT_EQUAL_UINT16(799, ui::PanelX(479, 799));
    TEST_ASSERT_EQUAL_UINT16(0, ui::PanelY(479, 799));
}

void test_ui_to_panel_every_pixel_once(void) {
    std::vector<uint8_t> portrait(ui::kScreenBytes, 0xFF);
    std::vector<uint8_t> frame(epd::kFrameBytes);
    const int dots[][2] = {{0, 0}, {479, 0}, {0, 799}, {479, 799}, {7, 8}, {8, 7}, {123, 456}};
    for (const auto &d : dots) {
        SetBlack(portrait, ui::kRowBytes, d[0], d[1]);
    }
    ui::ToPanel(portrait.data(), frame.data());
    int black = 0;
    for (int y = 0; y < epd::kHeight; ++y) {
        for (int x = 0; x < epd::kWidth; ++x) {
            black += White(frame, epd::kRowBytes, x, y) ? 0 : 1;
        }
    }
    TEST_ASSERT_EQUAL_INT(7, black);
    for (const auto &d : dots) {
        const auto x = static_cast<uint16_t>(d[0]);
        const auto y = static_cast<uint16_t>(d[1]);
        TEST_ASSERT_FALSE(White(frame, epd::kRowBytes, ui::PanelX(x, y), ui::PanelY(x, y)));
    }
}

void test_ui_refresh_plan(void) {
    ui::RefreshPlan plan;
    TEST_ASSERT_TRUE(plan.NextIsFull());
    for (int i = 1; i < ui::RefreshPlan::kFullEvery; ++i) {
        TEST_ASSERT_FALSE(plan.NextIsFull());
    }
    TEST_ASSERT_TRUE(plan.NextIsFull());
    TEST_ASSERT_FALSE(plan.NextIsFull());
}

}  // namespace

void RegisterUiTests(void) {
    RUN_TEST(test_ui_sizes);
    RUN_TEST(test_ui_corners);
    RUN_TEST(test_ui_to_panel_every_pixel_once);
    RUN_TEST(test_ui_refresh_plan);
}
