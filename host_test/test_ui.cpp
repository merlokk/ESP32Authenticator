// The portrait screen on the landscape panel, the full-refresh plan
// (components/ui/ui_logic.h); the PIN pad (pin_pad_logic.h).

#include <cstring>
#include <vector>

#include "pin_pad_logic.h"
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
    // A new screen is a full refresh, and the count starts over from it.
    TEST_ASSERT_FALSE(plan.NextIsFull(ui::RefreshPlan::kBigChange - 1));
    TEST_ASSERT_TRUE(plan.NextIsFull(ui::RefreshPlan::kBigChange));
    for (int i = 1; i < ui::RefreshPlan::kFullEvery; ++i) {
        TEST_ASSERT_FALSE(plan.NextIsFull(10));
    }
    TEST_ASSERT_TRUE(plan.NextIsFull(10));
}

void test_ui_changed_pixels(void) {
    const uint8_t a[3] = {0xFF, 0x00, 0xA5};
    const uint8_t b[3] = {0xFF, 0x81, 0x5A};
    TEST_ASSERT_EQUAL_UINT32(10, ui::ChangedPixels(a, b, 3));
    TEST_ASSERT_EQUAL_UINT32(0, ui::ChangedPixels(a, a, 3));
}

void test_ui_touch_to_screen(void) {
    // The sensor's corners land on the screen's corners (measured on the X4 Pro).
    ui::ScreenPoint p = ui::TouchToScreen(0, 0);
    TEST_ASSERT_EQUAL_UINT16(0, p.x);
    TEST_ASSERT_EQUAL_UINT16(0, p.y);
    p = ui::TouchToScreen(479, 799);
    TEST_ASSERT_EQUAL_UINT16(479, p.x);
    TEST_ASSERT_EQUAL_UINT16(799, p.y);
    p = ui::TouchToScreen(100, 700);
    TEST_ASSERT_EQUAL_UINT16(100, p.x);
    TEST_ASSERT_EQUAL_UINT16(700, p.y);
    // Past the edge: clamped onto it.
    p = ui::TouchToScreen(480, 800);
    TEST_ASSERT_EQUAL_UINT16(479, p.x);
    TEST_ASSERT_EQUAL_UINT16(799, p.y);
}

namespace pp = ui::pinpad;

pp::Step Type(pp::Pad *pad, const char *digits) {
    pp::Step last = pp::Step::kNone;
    for (const char *c = digits; *c != '\0'; ++c) {
        last = pad->Apply({pp::Key::kDigit, *c});
    }
    return last;
}

void test_pin_pad_keys(void) {
    TEST_ASSERT_EQUAL_CHAR('1', pp::KeyAt(0).digit);
    TEST_ASSERT_EQUAL_CHAR('9', pp::KeyAt(8).digit);
    TEST_ASSERT_TRUE(pp::KeyAt(9).key == pp::Key::kClear);
    TEST_ASSERT_EQUAL_CHAR('0', pp::KeyAt(10).digit);
    TEST_ASSERT_TRUE(pp::KeyAt(11).key == pp::Key::kBack);
    TEST_ASSERT_TRUE(pp::KeyAt(12).key == pp::Key::kNone);
    TEST_ASSERT_TRUE(pp::KeyAt(0xFFFF).key == pp::Key::kNone);  // LV_BUTTONMATRIX_BUTTON_NONE
}

void test_pin_pad_enter(void) {
    pp::Pad pad;
    pad.Start(false);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kEnter);
    TEST_ASSERT_TRUE(pad.Apply({pp::Key::kBack, 0}) == pp::Step::kNone);  // nothing to erase
    TEST_ASSERT_TRUE(Type(&pad, "12") == pp::Step::kTyped);
    TEST_ASSERT_TRUE(pad.Apply({pp::Key::kBack, 0}) == pp::Step::kTyped);
    TEST_ASSERT_EQUAL_UINT8(1, pad.count());
    TEST_ASSERT_TRUE(pad.Apply({pp::Key::kClear, 0}) == pp::Step::kTyped);
    TEST_ASSERT_EQUAL_UINT8(0, pad.count());
    TEST_ASSERT_TRUE(Type(&pad, "12345") == pp::Step::kTyped);
    TEST_ASSERT_TRUE(Type(&pad, "6") == pp::Step::kCheck);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kChecking);
    TEST_ASSERT_EQUAL_STRING("123456", pad.Pin());
    // While it is checked, keys do nothing.
    TEST_ASSERT_TRUE(Type(&pad, "7") == pp::Step::kNone);
    TEST_ASSERT_TRUE(pad.Apply({pp::Key::kClear, 0}) == pp::Step::kNone);
    TEST_ASSERT_EQUAL_STRING("123456", pad.Pin());
    // Wrong: asked again, the digits gone.
    pad.Done(false);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kEnter);
    TEST_ASSERT_EQUAL_UINT8(0, pad.count());
    TEST_ASSERT_EQUAL_STRING("", pad.Pin());
}

void test_pin_pad_new_pin_twice(void) {
    pp::Pad pad;
    pad.Start(true);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kNew);
    TEST_ASSERT_TRUE(Type(&pad, "246801") == pp::Step::kRepeat);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kRepeat);
    TEST_ASSERT_EQUAL_UINT8(0, pad.count());
    TEST_ASSERT_EQUAL_STRING("", pad.Pin());  // the first one is not shown again
    // A different one: a new PIN from the start.
    TEST_ASSERT_TRUE(Type(&pad, "246802") == pp::Step::kMismatch);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kNew);
    TEST_ASSERT_EQUAL_UINT8(0, pad.count());
    TEST_ASSERT_TRUE(Type(&pad, "246801") == pp::Step::kRepeat);
    TEST_ASSERT_TRUE(Type(&pad, "246801") == pp::Step::kCheck);
    TEST_ASSERT_EQUAL_STRING("246801", pad.Pin());
    // Refused (too simple, say): a new one again.
    pad.Done(true);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kNew);
}

void test_pin_pad_texts(void) {
    TEST_ASSERT_EQUAL_UINT8(9, pp::TriesLeft(1, 10));
    TEST_ASSERT_EQUAL_UINT8(0, pp::TriesLeft(10, 10));
    TEST_ASSERT_EQUAL_UINT8(0, pp::TriesLeft(12, 10));
    TEST_ASSERT_EQUAL_UINT32(1, pp::MinutesUp(1));
    TEST_ASSERT_EQUAL_UINT32(1, pp::MinutesUp(60));
    TEST_ASSERT_EQUAL_UINT32(10, pp::MinutesUp(599));
}

}  // namespace

void RegisterUiTests(void) {
    RUN_TEST(test_ui_sizes);
    RUN_TEST(test_ui_corners);
    RUN_TEST(test_ui_to_panel_every_pixel_once);
    RUN_TEST(test_ui_refresh_plan);
    RUN_TEST(test_ui_changed_pixels);
    RUN_TEST(test_ui_touch_to_screen);
    RUN_TEST(test_pin_pad_keys);
    RUN_TEST(test_pin_pad_enter);
    RUN_TEST(test_pin_pad_new_pin_twice);
    RUN_TEST(test_pin_pad_texts);
}
