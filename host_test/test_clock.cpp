// clock_math: civil date <-> Unix time, the believable range, the timer
// clock's state across a software reset and a power cut.

#include "clock_math.h"
#include "unity.h"

using rtc::DateTime;

namespace {

constexpr int64_t kUs = 1000000;

void test_clock_known_dates(void) {
    TEST_ASSERT_EQUAL_INT64(0, rtc::ToUnix({1970, 1, 1, 0, 0, 0}));
    TEST_ASSERT_EQUAL_INT64(951782400, rtc::ToUnix({2000, 2, 29, 0, 0, 0}));
    TEST_ASSERT_EQUAL_INT64(1704067200, rtc::ToUnix({2024, 1, 1, 0, 0, 0}));
    TEST_ASSERT_EQUAL_INT64(1791991323, rtc::ToUnix({2026, 10, 14, 15, 22, 3}));
    TEST_ASSERT_EQUAL_INT64(4102444799, rtc::ToUnix({2099, 12, 31, 23, 59, 59}));
}

void test_clock_round_trip_every_day(void) {
    // Every day 2024..2099 and back, at a time with every field non-zero.
    for (int64_t day = rtc::DaysFromCivil(2024, 1, 1); day < rtc::DaysFromCivil(2100, 1, 1);
         ++day) {
        const int64_t seconds = day * 86400 + 13 * 3600 + 47 * 60 + 9;
        const DateTime t = rtc::FromUnix(seconds);
        TEST_ASSERT_TRUE(rtc::Valid(t));
        TEST_ASSERT_EQUAL_INT64(seconds, rtc::ToUnix(t));
    }
}

void test_clock_validates_dates(void) {
    TEST_ASSERT_TRUE(rtc::Valid({2024, 2, 29, 23, 59, 59}));
    TEST_ASSERT_FALSE(rtc::Valid({2025, 2, 29, 0, 0, 0}));
    TEST_ASSERT_FALSE(rtc::Valid({2100, 2, 29, 0, 0, 0}));  // not a leap year
    TEST_ASSERT_TRUE(rtc::Valid({2000, 2, 29, 0, 0, 0}));
    TEST_ASSERT_FALSE(rtc::Valid({2024, 4, 31, 0, 0, 0}));
    TEST_ASSERT_FALSE(rtc::Valid({2024, 13, 1, 0, 0, 0}));
    TEST_ASSERT_FALSE(rtc::Valid({2024, 1, 0, 0, 0, 0}));
    TEST_ASSERT_FALSE(rtc::Valid({2024, 1, 1, 24, 0, 0}));
    TEST_ASSERT_FALSE(rtc::Valid({2024, 1, 1, 0, 60, 0}));
    TEST_ASSERT_FALSE(rtc::Valid({2024, 1, 1, 0, 0, 60}));
}

void test_clock_believable_range(void) {
    TEST_ASSERT_FALSE(rtc::Believable(0));  // an unset clock
    TEST_ASSERT_FALSE(rtc::Believable(rtc::ToUnix({2023, 12, 31, 23, 59, 59})));
    TEST_ASSERT_TRUE(rtc::Believable(rtc::ToUnix({2024, 1, 1, 0, 0, 0})));
    TEST_ASSERT_TRUE(rtc::Believable(rtc::ToUnix({2099, 12, 31, 23, 59, 59})));
    TEST_ASSERT_FALSE(rtc::Believable(rtc::ToUnix({2100, 1, 1, 0, 0, 0})));
}

void test_clock_timer_counts_from_when_it_was_set(void) {
    const int64_t set_at = rtc::ToUnix({2026, 10, 14, 12, 0, 0}) * kUs;
    const rtc::TimerState s = rtc::TimerSet(set_at, 5 * kUs);
    int64_t utc = 0;
    TEST_ASSERT_TRUE(rtc::TimerRead(s, 5 * kUs, &utc));
    TEST_ASSERT_EQUAL_INT64(set_at, utc);
    // A software reset later: the timer kept running, so does the time.
    TEST_ASSERT_TRUE(rtc::TimerRead(s, 3605 * kUs + 250, &utc));
    TEST_ASSERT_EQUAL_INT64(set_at + 3600 * kUs + 250, utc);
}

void test_clock_timer_that_restarted_has_no_time(void) {
    // Power-on: the timer starts from zero again, below the saved value.
    const rtc::TimerState s = rtc::TimerSet(rtc::ToUnix({2026, 1, 1, 0, 0, 0}) * kUs, 900 * kUs);
    int64_t utc = 123;
    TEST_ASSERT_FALSE(rtc::TimerRead(s, 2 * kUs, &utc));
    TEST_ASSERT_EQUAL_INT64(123, utc);  // untouched
}

void test_clock_timer_rejects_memory_that_is_not_ours(void) {
    rtc::TimerState s = rtc::TimerSet(rtc::ToUnix({2026, 1, 1, 0, 0, 0}) * kUs, 0);
    int64_t utc = 0;
    rtc::TimerState zero = {};
    TEST_ASSERT_FALSE(rtc::TimerRead(zero, 10, &utc));
    rtc::TimerState flipped = s;
    flipped.utc_us_at_set ^= 1LL << 40;  // one bit lost
    TEST_ASSERT_FALSE(rtc::TimerRead(flipped, 10, &utc));
    flipped = s;
    flipped.magic ^= 1;
    TEST_ASSERT_FALSE(rtc::TimerRead(flipped, 10, &utc));
    flipped = s;
    flipped.timer_us_at_set += 1;
    TEST_ASSERT_FALSE(rtc::TimerRead(flipped, 10, &utc));
    TEST_ASSERT_TRUE(rtc::TimerRead(s, 10, &utc));
}

}  // namespace

void RegisterClockTests(void) {
    RUN_TEST(test_clock_known_dates);
    RUN_TEST(test_clock_round_trip_every_day);
    RUN_TEST(test_clock_validates_dates);
    RUN_TEST(test_clock_believable_range);
    RUN_TEST(test_clock_timer_counts_from_when_it_was_set);
    RUN_TEST(test_clock_timer_that_restarted_has_no_time);
    RUN_TEST(test_clock_timer_rejects_memory_that_is_not_ours);
}
