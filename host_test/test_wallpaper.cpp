// Power-off wallpaper: file names, grey, the dither (components/ui/
// wallpaper_logic.h); the pins held through deep sleep (components/power/
// power_logic.h).

#include <vector>

#include "power_logic.h"
#include "unity.h"
#include "wallpaper_logic.h"
#include "dev_board.h"
#include "x4_pro.h"

namespace {

using ui::wallpaper::Dither;

bool White(const std::vector<uint8_t> &s, int x, int y) {
    return (s[y * ui::kRowBytes + x / 8] & (0x80 >> (x % 8))) != 0;
}

int CountWhite(const std::vector<uint8_t> &s) {
    int n = 0;
    for (int y = 0; y < ui::kHeight; ++y) {
        for (int x = 0; x < ui::kWidth; ++x) {
            n += White(s, x, y) ? 1 : 0;
        }
    }
    return n;
}

Dither dither;  // 2.9 KB: not on the test's stack

void test_wallpaper_names(void) {
    TEST_ASSERT_TRUE(ui::wallpaper::IsImageName("safe-480x800.png"));
    TEST_ASSERT_TRUE(ui::wallpaper::IsImageName("SAFE.PNG"));
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName("._safe.png"));  // macOS
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName(".png"));
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName("safe.jpg"));
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName("png"));
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName(""));
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName(nullptr));
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName("../.authenticator/x.png"));
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName("sub/a.png"));
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName("a\\b.png"));
    TEST_ASSERT_FALSE(ui::wallpaper::IsImageName("a\nb.png"));
}

void test_wallpaper_luma(void) {
    TEST_ASSERT_EQUAL_UINT8(255, ui::wallpaper::Luma(255, 255, 255, 255));
    TEST_ASSERT_EQUAL_UINT8(0, ui::wallpaper::Luma(0, 0, 0, 255));
    TEST_ASSERT_EQUAL_UINT8(255, ui::wallpaper::Luma(0, 0, 0, 0));  // transparent: white
    TEST_ASSERT_EQUAL_UINT8(100, ui::wallpaper::Luma(100, 100, 100, 255));
}

void test_wallpaper_flat_tones(void) {
    std::vector<uint8_t> screen(ui::kScreenBytes);
    std::vector<uint8_t> grey(ui::kWidth * ui::kHeight, 255);
    dither.Run(grey.data(), ui::kWidth, ui::kHeight, screen.data());
    TEST_ASSERT_EQUAL_INT(ui::kWidth * ui::kHeight, CountWhite(screen));
    grey.assign(grey.size(), 0);
    dither.Run(grey.data(), ui::kWidth, ui::kHeight, screen.data());
    TEST_ASSERT_EQUAL_INT(0, CountWhite(screen));
    // Light grey: mostly white, some black dots.
    grey.assign(grey.size(), 200);
    dither.Run(grey.data(), ui::kWidth, ui::kHeight, screen.data());
    const int white = CountWhite(screen);
    TEST_ASSERT_TRUE(white > ui::kWidth * ui::kHeight * 6 / 10);
    TEST_ASSERT_TRUE(white < ui::kWidth * ui::kHeight * 95 / 100);
}

void test_wallpaper_centred(void) {
    // A 2x2 black picture lands in the middle; the rest is white.
    std::vector<uint8_t> screen(ui::kScreenBytes);
    const uint8_t black[4] = {0, 0, 0, 0};
    dither.Run(black, 2, 2, screen.data());
    TEST_ASSERT_EQUAL_INT(ui::kWidth * ui::kHeight - 4, CountWhite(screen));
    TEST_ASSERT_FALSE(White(screen, 239, 399));
    TEST_ASSERT_FALSE(White(screen, 240, 400));
    // Larger than the screen: cropped, its middle shown.
    std::vector<uint8_t> big(500 * 820, 255);
    big[(10 + 0) * 500 + 10] = 0;  // screen (0, 0)
    dither.Run(big.data(), 500, 820, screen.data());
    TEST_ASSERT_FALSE(White(screen, 0, 0));
    TEST_ASSERT_EQUAL_INT(ui::kWidth * ui::kHeight - 1, CountWhite(screen));
}

void test_power_sleep_holds(void) {
    constexpr boards::X4Pro b;
    constexpr power::Holds h = power::SleepHolds(b);
    TEST_ASSERT_EQUAL_size_t(3, h.size);
    TEST_ASSERT_EQUAL_INT8(14, h.items[0].pin);  // EPD RST high: the panel stays asleep
    TEST_ASSERT_EQUAL_UINT8(1, h.items[0].level);
    TEST_ASSERT_EQUAL_INT8(5, h.items[1].pin);   // SD power off
    TEST_ASSERT_EQUAL_UINT8(1, h.items[1].level);
    TEST_ASSERT_EQUAL_INT8(2, h.items[2].pin);   // touch power off
    TEST_ASSERT_EQUAL_UINT8(1, h.items[2].level);
    constexpr boards::DevBoard none;  // no panel, SD or touch
    TEST_ASSERT_EQUAL_size_t(0, power::SleepHolds(none).size);
}

}  // namespace

void RegisterWallpaperTests(void) {
    RUN_TEST(test_wallpaper_names);
    RUN_TEST(test_wallpaper_luma);
    RUN_TEST(test_wallpaper_flat_tones);
    RUN_TEST(test_wallpaper_centred);
    RUN_TEST(test_power_sleep_holds);
}
