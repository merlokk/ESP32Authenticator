// Frontlight curve and cool/warm mix (devices/frontlight/frontlight_logic.h).

#include "frontlight_logic.h"
#include "unity.h"

namespace {

namespace fl = frontlight;

void test_frontlight_curve(void) {
    TEST_ASSERT_EQUAL_UINT16(0, fl::kCurve[0]);
    TEST_ASSERT_EQUAL_UINT16(1, fl::kCurve[1]);  // the dimmest light: 1 LSB
    TEST_ASSERT_EQUAL_UINT16(fl::kFullDuty, fl::kCurve[100]);
    // Every percent is a step of its own.
    for (int p = 1; p < 100; ++p) {
        TEST_ASSERT_TRUE(fl::kCurve[p] < fl::kCurve[p + 1]);
    }
}

void test_frontlight_mix_keeps_the_total(void) {
    for (int b = 0; b <= 100; ++b) {
        for (int w = 0; w <= 100; w += 5) {
            const fl::Duties d = fl::Mix(static_cast<uint8_t>(b), static_cast<uint8_t>(w));
            TEST_ASSERT_EQUAL_UINT32(fl::kCurve[b], static_cast<uint32_t>(d.cool) + d.warm);
        }
    }
}

void test_frontlight_mix_ends_and_middle(void) {
    fl::Duties d = fl::Mix(100, 0);
    TEST_ASSERT_EQUAL_UINT16(fl::kFullDuty, d.cool);
    TEST_ASSERT_EQUAL_UINT16(0, d.warm);
    d = fl::Mix(100, 100);
    TEST_ASSERT_EQUAL_UINT16(0, d.cool);
    TEST_ASSERT_EQUAL_UINT16(fl::kFullDuty, d.warm);
    d = fl::Mix(0, 50);
    TEST_ASSERT_EQUAL_UINT16(0, d.cool);
    TEST_ASSERT_EQUAL_UINT16(0, d.warm);
    // The dimmest light at a neutral tint is still light.
    d = fl::Mix(1, 50);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(d.cool) + d.warm);
}

void test_frontlight_mix_clamps(void) {
    const fl::Duties d = fl::Mix(255, 255);
    TEST_ASSERT_EQUAL_UINT16(0, d.cool);
    TEST_ASSERT_EQUAL_UINT16(fl::kFullDuty, d.warm);
}

}  // namespace

void RegisterFrontlightTests(void) {
    RUN_TEST(test_frontlight_curve);
    RUN_TEST(test_frontlight_mix_keeps_the_total);
    RUN_TEST(test_frontlight_mix_ends_and_middle);
    RUN_TEST(test_frontlight_mix_clamps);
}
