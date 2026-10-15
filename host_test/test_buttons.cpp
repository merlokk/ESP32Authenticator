// Button debounce and short/long press (devices/buttons/button_logic.h).
// Ported from approver-esp32 (host_test/test_buttons.cpp).

#include "button_logic.h"
#include "unity.h"

namespace {

using buttons::Debounce;
using buttons::Event;
using buttons::kDebounceMs;
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
}
