// BM8563 RTC (devices/clock/bm8563_logic.h): BCD, the counters <-> UTC, VL,
// when to start the chip after a write.

#include "bm8563_logic.h"
#include "unity.h"

namespace {

namespace bm = rtc::bm8563;

constexpr int64_t kUs = 1000000;

void AssertDecoded(bm::Decoded expected, bm::Decoded actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

void test_rtc_bcd(void) {
    TEST_ASSERT_EQUAL_UINT8(0x59, bm::ToBcd(59));
    TEST_ASSERT_EQUAL_UINT8(59, bm::FromBcd(0x59));
    TEST_ASSERT_TRUE(bm::IsBcd(0x99));
    TEST_ASSERT_FALSE(bm::IsBcd(0x1A));
    TEST_ASSERT_FALSE(bm::IsBcd(0xA1));
}

void test_rtc_encode_decode_round_trip(void) {
    // 2026-10-14 15:22:03 UTC, a Wednesday.
    const int64_t t = 1791991323;
    uint8_t raw[bm::kCounterCount] = {};
    bm::Encode(t, raw);
    const uint8_t expected[bm::kCounterCount] = {0x03, 0x22, 0x15, 0x14, 3, 0x10, 0x26};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, raw, bm::kCounterCount);
    rtc::DateTime out = {};
    AssertDecoded(bm::Decoded::kOk, bm::Decode(raw, &out));
    TEST_ASSERT_EQUAL_INT64(t, rtc::ToUnix(out));
}

void test_rtc_round_trip_every_day(void) {
    for (int64_t day = rtc::DaysFromCivil(2024, 1, 1); day < rtc::DaysFromCivil(2100, 1, 1);
         ++day) {
        const int64_t t = day * 86400 + 23 * 3600 + 59 * 60 + 59;
        uint8_t raw[bm::kCounterCount] = {};
        bm::Encode(t, raw);
        rtc::DateTime out = {};
        AssertDecoded(bm::Decoded::kOk, bm::Decode(raw, &out));
        TEST_ASSERT_EQUAL_INT64(t, rtc::ToUnix(out));
    }
}

void test_rtc_weekday(void) {
    TEST_ASSERT_EQUAL_UINT8(4, bm::Weekday(0));                          // 1970-01-01 Thu
    TEST_ASSERT_EQUAL_UINT8(1, bm::Weekday(rtc::ToUnix({2024, 1, 1, 12, 0, 0})));  // Mon
    TEST_ASSERT_EQUAL_UINT8(0, bm::Weekday(rtc::ToUnix({2026, 10, 18, 0, 0, 0})));  // Sun
}

void test_rtc_ignores_unused_bits(void) {
    // Unused bits read as anything; the century flag is ignored (20xx).
    const uint8_t raw[bm::kCounterCount] = {0x03, 0x80 | 0x22, 0xC0 | 0x15, 0xC0 | 0x14,
                                            0xF8 | 3, 0x60 | bm::kMonthsCentury | 0x10, 0x26};
    rtc::DateTime out = {};
    AssertDecoded(bm::Decoded::kOk, bm::Decode(raw, &out));
    TEST_ASSERT_EQUAL_INT64(1791991323, rtc::ToUnix(out));
}

void test_rtc_voltage_low_is_no_time(void) {
    uint8_t raw[bm::kCounterCount] = {};
    bm::Encode(1791991323, raw);
    raw[0] |= bm::kSecondsVl;
    rtc::DateTime out = {};
    AssertDecoded(bm::Decoded::kVoltageLow, bm::Decode(raw, &out));
}

void test_rtc_bad_fields_are_no_time(void) {
    rtc::DateTime out = {};
    // Not BCD.
    const uint8_t not_bcd[bm::kCounterCount] = {0x0A, 0x00, 0x00, 0x01, 0, 0x01, 0x26};
    AssertDecoded(bm::Decoded::kBadFields, bm::Decode(not_bcd, &out));
    // BCD, but February 30th.
    const uint8_t no_date[bm::kCounterCount] = {0x00, 0x00, 0x00, 0x30, 0, 0x02, 0x26};
    AssertDecoded(bm::Decoded::kBadFields, bm::Decode(no_date, &out));
    // A date, but before the believable range (a reset chip: 2000-01-01).
    const uint8_t reset[bm::kCounterCount] = {0x00, 0x00, 0x00, 0x01, 6, 0x01, 0x00};
    AssertDecoded(bm::Decoded::kBadFields, bm::Decode(reset, &out));
}

void test_rtc_plan_write(void) {
    const int64_t base = 1791991323;
    for (int64_t us = 0; us < kUs; us += 1000) {
        const int64_t now = base * kUs + us;
        const bm::WritePlan p = bm::PlanWrite(now, 50000);
        // At least the lead away, and no more than a second after that.
        TEST_ASSERT_TRUE(p.start_at_us >= now + 50000);
        TEST_ASSERT_TRUE(p.start_at_us < now + 50000 + kUs);
        // The chip ticks to seconds + 1 exactly when that second begins...
        TEST_ASSERT_EQUAL_INT64((p.seconds + 1) * kUs, p.start_at_us + bm::kFirstTickUs);
        // ...and until then shows the second the start falls in.
        TEST_ASSERT_TRUE(p.seconds * kUs <= p.start_at_us);
    }
    // With the measured 1 s: started on a second boundary, showing it.
    const bm::WritePlan p = bm::PlanWrite(base * kUs + 100000, 50000);
    TEST_ASSERT_EQUAL_INT64(base + 1, p.seconds);
    TEST_ASSERT_EQUAL_INT64((base + 1) * kUs, p.start_at_us);
}

}  // namespace

void RegisterRtcTests(void) {
    RUN_TEST(test_rtc_bcd);
    RUN_TEST(test_rtc_encode_decode_round_trip);
    RUN_TEST(test_rtc_round_trip_every_day);
    RUN_TEST(test_rtc_weekday);
    RUN_TEST(test_rtc_ignores_unused_bits);
    RUN_TEST(test_rtc_voltage_low_is_no_time);
    RUN_TEST(test_rtc_bad_fields_are_no_time);
    RUN_TEST(test_rtc_plan_write);
}
