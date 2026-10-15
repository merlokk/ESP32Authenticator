// Button debounce, short/long and single/double press
// (devices/buttons/button_logic.h).
// Ported from approver-esp32 (host_test/test_buttons.cpp).

#include "button_logic.h"
#include "unity.h"

namespace {

using buttons::Debounce;
using buttons::Event;
using buttons::kDebounceMs;
using buttons::MultiPress;
using buttons::Press;
using buttons::PressLength;

void AssertEvent(Event expected, Event actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

void AssertPress(Press expected, Press actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

// --- Debounce

void test_debounce_adopts_a_level_without_an_edge(void) {
    Debounce d;
    d.Reset(true, 1000);
    TEST_ASSERT_TRUE(d.Pressed());
    AssertEvent(Event::kNone, d.Update(true, 1000 + kDebounceMs * 2));
}

void test_debounce_waits_out_the_window(void) {
    Debounce d;
    d.Reset(false, 0);
    // The window starts when the new level is first seen.
    AssertEvent(Event::kNone, d.Update(true, 1));
    AssertEvent(Event::kNone, d.Update(true, 1 + kDebounceMs - 1));
    TEST_ASSERT_FALSE(d.Pressed());
    AssertEvent(Event::kPressed, d.Update(true, 1 + kDebounceMs));
    TEST_ASSERT_TRUE(d.Pressed());
}

void test_debounce_reports_a_press_once(void) {
    Debounce d;
    d.Reset(false, 0);
    d.Update(true, 0);
    d.Update(true, kDebounceMs);
    AssertEvent(Event::kNone, d.Update(true, kDebounceMs + 100));
    AssertEvent(Event::kNone, d.Update(true, kDebounceMs + 200));
}

void test_debounce_swallows_a_bounce_shorter_than_the_window(void) {
    Debounce d;
    d.Reset(false, 0);
    for (uint32_t t = 1; t < kDebounceMs; t += 2) {
        AssertEvent(Event::kNone, d.Update(t % 4 == 1, t));
    }
    TEST_ASSERT_FALSE(d.Pressed());
}

void test_debounce_a_spike_that_settles_back_reports_nothing(void) {
    Debounce d;
    d.Reset(false, 0);
    d.Update(true, 1);
    d.Update(false, 5);
    AssertEvent(Event::kNone, d.Update(false, 5 + kDebounceMs * 2));
    TEST_ASSERT_FALSE(d.Pressed());
}

void test_debounce_reports_the_release(void) {
    Debounce d;
    d.Reset(false, 0);
    d.Update(true, 0);
    AssertEvent(Event::kPressed, d.Update(true, kDebounceMs));
    const uint32_t up = kDebounceMs + 500;
    AssertEvent(Event::kNone, d.Update(false, up));
    AssertEvent(Event::kReleased, d.Update(false, up + kDebounceMs));
    TEST_ASSERT_FALSE(d.Pressed());
}

void test_debounce_measures_how_long_the_state_held(void) {
    Debounce d;
    d.Reset(false, 1000);
    TEST_ASSERT_EQUAL_UINT32(500, d.StableMs(1500));
    d.Update(true, 2000);
    d.Update(true, 2000 + kDebounceMs);  // the edge
    TEST_ASSERT_EQUAL_UINT32(100, d.StableMs(2000 + kDebounceMs + 100));
}

void test_debounce_survives_the_millisecond_wrap(void) {
    constexpr uint32_t kNearWrap = 0xFFFFFFF0;
    Debounce d;
    d.Reset(false, kNearWrap);
    TEST_ASSERT_EQUAL_UINT32(16, d.StableMs(0));
    AssertEvent(Event::kNone, d.Update(true, kNearWrap + 1));
    AssertEvent(Event::kPressed, d.Update(true, kNearWrap + 1 + kDebounceMs));
    TEST_ASSERT_TRUE(d.Pressed());
}

// --- Short / long press

void test_press_released_early_is_short(void) {
    PressLength key(2000);
    AssertPress(Press::kNone, key.Update(true, 1000));
    AssertPress(Press::kNone, key.Update(true, 2500));
    AssertPress(Press::kShort, key.Update(false, 2900));
}

void test_press_long_fires_while_still_held(void) {
    PressLength key(2000);
    key.Update(true, 1000);
    AssertPress(Press::kNone, key.Update(true, 2999));
    AssertPress(Press::kLong, key.Update(true, 3000));
}

void test_press_long_fires_once_and_the_release_says_nothing(void) {
    PressLength key(2000);
    key.Update(true, 1000);
    AssertPress(Press::kLong, key.Update(true, 3000));
    AssertPress(Press::kNone, key.Update(true, 4000));
    AssertPress(Press::kNone, key.Update(false, 5000));
}

void test_press_after_a_long_one_starts_over(void) {
    PressLength key(2000);
    key.Update(true, 1000);
    key.Update(true, 3000);
    key.Update(false, 3100);
    key.Update(true, 4000);
    AssertPress(Press::kShort, key.Update(false, 4200));
}

void test_press_untouched_reports_nothing(void) {
    PressLength key(2000);
    for (uint32_t t = 0; t < 10000; t += 20) {
        AssertPress(Press::kNone, key.Update(false, t));
    }
    TEST_ASSERT_EQUAL_UINT32(0, key.HeldMs(10000));
}

void test_press_hold_survives_the_millisecond_wrap(void) {
    PressLength key(2000);
    key.Update(true, 0xFFFFFF00u);
    TEST_ASSERT_EQUAL_UINT32(656, key.HeldMs(400));
    AssertPress(Press::kLong, key.Update(true, 0xFFFFFF00u + 2000));
}

// --- Single / double press

// A tap from `at` lasting `len` ms; returns what the release reported.
uint8_t Tap(MultiPress &m, uint32_t at, uint32_t len = 80) {
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(true, at));
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(true, at + len / 2));
    return m.Update(false, at + len);
}

void test_multi_single_is_reported_after_the_gap(void) {
    MultiPress m(400, 500);
    TEST_ASSERT_EQUAL_UINT8(0, Tap(m, 1000));
    TEST_ASSERT_EQUAL_UINT8(1, m.Pending());
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 1080 + 399));
    TEST_ASSERT_EQUAL_UINT8(1, m.Update(false, 1080 + 400));
    // Once.
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 1080 + 800));
    TEST_ASSERT_EQUAL_UINT8(0, m.Pending());
}

void test_multi_double_inside_the_gap(void) {
    MultiPress m(400, 500);
    Tap(m, 1000);
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 1200));
    TEST_ASSERT_EQUAL_UINT8(0, Tap(m, 1300));
    TEST_ASSERT_EQUAL_UINT8(2, m.Pending());
    TEST_ASSERT_EQUAL_UINT8(2, m.Update(false, 1380 + 400));
}

void test_multi_triple(void) {
    MultiPress m(400, 500);
    Tap(m, 1000);
    Tap(m, 1200);
    Tap(m, 1400);
    TEST_ASSERT_EQUAL_UINT8(3, m.Update(false, 1480 + 400));
}

void test_multi_presses_further_apart_are_two_singles(void) {
    MultiPress m(400, 500);
    Tap(m, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, m.Update(false, 1080 + 400));
    Tap(m, 2000);
    TEST_ASSERT_EQUAL_UINT8(1, m.Update(false, 2080 + 400));
}

void test_multi_a_long_press_is_not_a_tap(void) {
    MultiPress m(400, 500);
    m.Update(true, 1000);
    m.Update(true, 1600);  // past the longest tap
    TEST_ASSERT_EQUAL_UINT8(0, m.Pending());
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 2000));
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 3000));
}

void test_multi_a_long_press_cancels_the_series(void) {
    MultiPress m(400, 500);
    Tap(m, 1000);
    // Second press held too long: neither a single nor a double.
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(true, 1200));
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(true, 1800));
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 1900));
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 3000));
}

void test_multi_a_long_press_seen_only_at_its_ends_is_not_a_tap(void) {
    // Polled at press and release only.
    MultiPress m(400, 500);
    m.Update(true, 1000);
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 1700));
    TEST_ASSERT_EQUAL_UINT8(0, m.Pending());
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 3000));
}

void test_multi_the_gap_is_not_counted_while_held(void) {
    MultiPress m(400, 500);
    Tap(m, 1000);
    // Second press starts inside the gap and is held past it (but is a tap).
    m.Update(true, 1300);
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(true, 1700));
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 1750));
    TEST_ASSERT_EQUAL_UINT8(2, m.Update(false, 1750 + 400));
}

void test_multi_survives_the_millisecond_wrap(void) {
    MultiPress m(400, 500);
    Tap(m, 0xFFFFFF00u);
    Tap(m, 0xFFFFFF00u + 200);  // released at 0xFFFFFF00 + 280: 0x18 after the wrap
    TEST_ASSERT_EQUAL_UINT8(0, m.Update(false, 0x18 + 399));
    TEST_ASSERT_EQUAL_UINT8(2, m.Update(false, 0x18 + 400));
}

}  // namespace

void RegisterButtonTests(void) {
    RUN_TEST(test_debounce_adopts_a_level_without_an_edge);
    RUN_TEST(test_debounce_waits_out_the_window);
    RUN_TEST(test_debounce_reports_a_press_once);
    RUN_TEST(test_debounce_swallows_a_bounce_shorter_than_the_window);
    RUN_TEST(test_debounce_a_spike_that_settles_back_reports_nothing);
    RUN_TEST(test_debounce_reports_the_release);
    RUN_TEST(test_debounce_measures_how_long_the_state_held);
    RUN_TEST(test_debounce_survives_the_millisecond_wrap);

    RUN_TEST(test_press_released_early_is_short);
    RUN_TEST(test_press_long_fires_while_still_held);
    RUN_TEST(test_press_long_fires_once_and_the_release_says_nothing);
    RUN_TEST(test_press_after_a_long_one_starts_over);
    RUN_TEST(test_press_untouched_reports_nothing);
    RUN_TEST(test_press_hold_survives_the_millisecond_wrap);

    RUN_TEST(test_multi_single_is_reported_after_the_gap);
    RUN_TEST(test_multi_double_inside_the_gap);
    RUN_TEST(test_multi_triple);
    RUN_TEST(test_multi_presses_further_apart_are_two_singles);
    RUN_TEST(test_multi_a_long_press_is_not_a_tap);
    RUN_TEST(test_multi_a_long_press_cancels_the_series);
    RUN_TEST(test_multi_a_long_press_seen_only_at_its_ends_is_not_a_tap);
    RUN_TEST(test_multi_the_gap_is_not_counted_while_held);
    RUN_TEST(test_multi_survives_the_millisecond_wrap);
}
