// Power-off wallpaper: file names, grey, the dither (components/ui/
// wallpaper_logic.h); the pins held through deep sleep, the idle off and the
// key kept through a sleep (components/power/power_logic.h).

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

int Level(const std::vector<uint8_t> &low, const std::vector<uint8_t> &high, int x, int y) {
    return (White(low, x, y) ? 1 : 0) | (White(high, x, y) ? 2 : 0);
}

void test_wallpaper_grey_levels(void) {
    std::vector<uint8_t> low(ui::kScreenBytes);
    std::vector<uint8_t> high(ui::kScreenBytes);
    // The four tones exactly: four bands, no dither noise; the margin white.
    std::vector<uint8_t> grey(4 * 100, 0);
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 100; ++y) {
            grey[y * 4 + x] = static_cast<uint8_t>(x * 85);
        }
    }
    dither.Run(grey.data(), 4, 100, epd::kGreyLevels, low.data(), high.data());
    for (int x = 0; x < 4; ++x) {
        for (int y = 350; y < 450; ++y) {
            TEST_ASSERT_EQUAL_INT(x, Level(low, high, 238 + x, y));
        }
    }
    TEST_ASSERT_EQUAL_INT(3, Level(low, high, 0, 0));
    // Between two tones: a mix of those two only, about half each.
    grey.assign(ui::kWidth * ui::kHeight, 128);  // between dark 85 and light 170
    dither.Run(grey.data(), ui::kWidth, ui::kHeight, epd::kGreyLevels, low.data(), high.data());
    int counts[4] = {};
    for (int y = 0; y < ui::kHeight; ++y) {
        for (int x = 0; x < ui::kWidth; ++x) {
            ++counts[Level(low, high, x, y)];
        }
    }
    TEST_ASSERT_EQUAL_INT(0, counts[0]);
    TEST_ASSERT_EQUAL_INT(0, counts[3]);
    TEST_ASSERT_TRUE(counts[1] > ui::kWidth * ui::kHeight * 4 / 10);
    TEST_ASSERT_TRUE(counts[2] > ui::kWidth * ui::kHeight * 4 / 10);
    // Two levels: the one-plane Run, the high plane untouched.
    high.assign(high.size(), 0x5A);
    std::vector<uint8_t> screen(ui::kScreenBytes);
    dither.Run(grey.data(), ui::kWidth, ui::kHeight, screen.data());
    dither.Run(grey.data(), ui::kWidth, ui::kHeight, 2, low.data(), high.data());
    TEST_ASSERT_TRUE(screen == low);
    TEST_ASSERT_EQUAL_HEX8(0x5A, high[1234]);
}

void test_epd_grey_tables(void) {
    TEST_ASSERT_TRUE(epd::HasGrey(epd::Controller::kUc8179));
    TEST_ASSERT_FALSE(epd::HasGrey(epd::Controller::kUc8279));
    TEST_ASSERT_FALSE(epd::HasGrey(epd::Controller::kSsd1677));
    // FreeInk's gray_full header unpacked (configureDirectGrayscale).
    TEST_ASSERT_EQUAL_HEX8(0x52, epd::kUc8179GreyPower[0].cmd);
    TEST_ASSERT_EQUAL_HEX8(0x02, epd::kUc8179GreyPower[0].data[0]);  // (0x67 & 8) | (0xBF >> 6)
    TEST_ASSERT_EQUAL_HEX8(0x06, epd::kUc8179GreyPower[1].data[0]);  // 0x67 >> 4
    TEST_ASSERT_EQUAL_HEX8(0x07, epd::kUc8179GreyPower[2].data[1]);  // 0x67 & 7
    TEST_ASSERT_EQUAL_HEX8(0x3F, epd::kUc8179GreyPower[2].data[2]);  // 0xBF & 0x3F
    TEST_ASSERT_EQUAL_HEX8(0x26, epd::kUc8179GreyPower[4].data[0]);
    // Registers 0x21..0x24: white, light, dark, black (FreeInk rows 4, 2, 3, 1).
    TEST_ASSERT_EQUAL_HEX8(0x4A, epd::kUc8179GreyLuts[1][6]);
    TEST_ASSERT_EQUAL_HEX8(0x62, epd::kUc8179GreyLuts[2][6]);
    TEST_ASSERT_EQUAL_HEX8(0x60, epd::kUc8179GreyLuts[3][6]);
    TEST_ASSERT_EQUAL_HEX8(0x44, epd::kUc8179GreyLuts[4][6]);
}

void test_power_sleep_holds(void) {
    constexpr boards::X4Pro b;
    constexpr power::Holds h = power::SleepHolds(b);
    TEST_ASSERT_EQUAL_size_t(5, h.size);
    TEST_ASSERT_EQUAL_INT8(14, h.items[0].pin);  // EPD RST high: the panel stays asleep
    TEST_ASSERT_EQUAL_UINT8(1, h.items[0].level);
    TEST_ASSERT_EQUAL_INT8(5, h.items[1].pin);   // SD power off
    TEST_ASSERT_EQUAL_UINT8(1, h.items[1].level);
    TEST_ASSERT_EQUAL_INT8(2, h.items[2].pin);   // touch power off
    TEST_ASSERT_EQUAL_UINT8(1, h.items[2].level);
    TEST_ASSERT_EQUAL_INT8(8, h.items[3].pin);   // frontlight cool low
    TEST_ASSERT_EQUAL_UINT8(0, h.items[3].level);
    TEST_ASSERT_EQUAL_INT8(9, h.items[4].pin);   // frontlight warm low
    TEST_ASSERT_EQUAL_UINT8(0, h.items[4].level);
    constexpr boards::DevBoard none;  // no panel, SD or touch
    TEST_ASSERT_EQUAL_size_t(0, power::SleepHolds(none).size);
}

void test_power_idle_off(void) {
    TEST_ASSERT_FALSE(power::IdleOff(10 * 60000, 0, 0, false));  // 0: never
    TEST_ASSERT_FALSE(power::IdleOff(10 * 60000 - 1, 0, 10, false));
    TEST_ASSERT_TRUE(power::IdleOff(10 * 60000, 0, 10, false));
    TEST_ASSERT_FALSE(power::IdleOff(10 * 60000, 0, 10, true));  // held on
    TEST_ASSERT_FALSE(power::IdleOff(5, 6, 1, false));  // activity after the reading
    // Minutes beyond 32-bit milliseconds: an uptime of 60 days.
    constexpr uint64_t day = 24ull * 3600 * 1000;
    TEST_ASSERT_TRUE(power::IdleOff(60 * day, 60 * day - 1440ull * 60000, 1440, false));
}

void test_power_kept_secret(void) {
    const uint8_t secret[3] = {1, 2, 3};
    constexpr int64_t t = 1800000000;  // 2027
    power::Kept k = {};
    TEST_ASSERT_FALSE(power::FillKept(&k, secret, 3, t, 0));  // no hours
    TEST_ASSERT_FALSE(power::FillKept(&k, secret, 3, 0, 8));  // no clock
    uint8_t big[power::kKeptBytes + 1] = {};
    TEST_ASSERT_FALSE(power::FillKept(&k, big, sizeof big, t, 8));
    TEST_ASSERT_TRUE(power::FillKept(&k, secret, 3, t, 8));
    TEST_ASSERT_EQUAL_UINT32(3, k.size);
    TEST_ASSERT_EQUAL_UINT8(3, k.data[2]);
    TEST_ASSERT_TRUE(power::KeptUsable(k, t));
    TEST_ASSERT_TRUE(power::KeptUsable(k, t + 8 * 3600 - 1));
    TEST_ASSERT_FALSE(power::KeptUsable(k, t + 8 * 3600));  // out of time
    TEST_ASSERT_TRUE(power::KeptUsable(k, t - power::kKeptSlackSeconds));
    TEST_ASSERT_FALSE(power::KeptUsable(k, t - power::kKeptSlackSeconds - 1));  // clock moved back
    TEST_ASSERT_FALSE(power::KeptUsable(k, 0));  // the clock lost
    power::Kept bad = k;
    bad.data[0] ^= 1;  // RTC memory after a power loss
    TEST_ASSERT_FALSE(power::KeptUsable(bad, t));
    bad = k;
    bad.until_utc += 3600;  // longer than it was kept for
    TEST_ASSERT_FALSE(power::KeptUsable(bad, t));
    bad = k;
    bad.magic = 0;
    TEST_ASSERT_FALSE(power::KeptUsable(bad, t));
}

}  // namespace

void RegisterWallpaperTests(void) {
    RUN_TEST(test_wallpaper_names);
    RUN_TEST(test_wallpaper_luma);
    RUN_TEST(test_wallpaper_flat_tones);
    RUN_TEST(test_wallpaper_centred);
    RUN_TEST(test_wallpaper_grey_levels);
    RUN_TEST(test_epd_grey_tables);
    RUN_TEST(test_power_sleep_holds);
    RUN_TEST(test_power_idle_off);
    RUN_TEST(test_power_kept_secret);
}
