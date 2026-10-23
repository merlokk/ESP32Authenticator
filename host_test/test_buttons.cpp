// Button debounce, short/long and single/double press
// (devices/buttons/button_logic.h).
// Ported from approver-esp32 (host_test/test_buttons.cpp).

#include <initializer_list>

#include "button_logic.h"
#include "key_events.h"
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

// --- KeyTracker (key_events.h)

using buttons::Kind;
using buttons::KeyTracker;
using buttons::kLongMs;

// Every event from `from` to `to` (exclusive) in 1 ms steps at one level.
struct Seen {
    Kind kinds[16];
    uint8_t counts[16];
    int n = 0;
};

void Run(KeyTracker &k, bool raw, uint32_t from, uint32_t to, Seen &seen) {
    for (uint32_t t = from; t < to; ++t) {
        const buttons::KeyEvents e = k.Update(raw, t);
        for (uint8_t i = 0; i < e.size && seen.n < 16; ++i) {
            TEST_ASSERT_EQUAL_UINT8(7, e.items[i].key);
            seen.kinds[seen.n] = e.items[i].kind;
            seen.counts[seen.n++] = e.items[i].count;
        }
    }
}

void AssertKinds(const Seen &seen, std::initializer_list<Kind> expected) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected.size()), seen.n);
    int i = 0;
    for (const Kind k : expected) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(k), static_cast<int>(seen.kinds[i++]));
    }
}

void test_keys_single_click(void) {
    KeyTracker k(7);
    k.Reset(false, 0);
    Seen seen;
    Run(k, true, 0, 200, seen);
    Run(k, false, 200, 1000, seen);
    AssertKinds(seen, {Kind::kPressed, Kind::kReleased, Kind::kClick});
    TEST_ASSERT_EQUAL_UINT8(1, seen.counts[2]);
}

void test_keys_double_click(void) {
    KeyTracker k(7);
    k.Reset(false, 0);
    Seen seen;
    Run(k, true, 0, 100, seen);
    Run(k, false, 100, 300, seen);
    Run(k, true, 300, 400, seen);
    Run(k, false, 400, 1200, seen);
    AssertKinds(seen, {Kind::kPressed, Kind::kReleased, Kind::kPressed, Kind::kReleased,
                       Kind::kClick});
    TEST_ASSERT_EQUAL_UINT8(2, seen.counts[4]);
}

void test_keys_long_press_fires_while_held_and_is_no_click(void) {
    KeyTracker k(7);
    k.Reset(false, 0);
    Seen seen;
    // Debounced down at 25 ms: long at 25 + kLongMs, still held.
    Run(k, true, 0, 25 + kLongMs + 1, seen);
    AssertKinds(seen, {Kind::kPressed, Kind::kLong});
    Run(k, true, 25 + kLongMs + 1, 2000, seen);
    Run(k, false, 2000, 3000, seen);
    AssertKinds(seen, {Kind::kPressed, Kind::kLong, Kind::kReleased});
}

void test_keys_a_press_of_exactly_the_threshold_is_long_only(void) {
    KeyTracker k(7);
    k.Reset(false, 0);
    Seen seen;
    // Raw down 0..kLongMs+25: debounced 25..kLongMs+50, held kLongMs exactly.
    Run(k, true, 0, kLongMs + 25, seen);
    Run(k, false, kLongMs + 25, kLongMs + 2000, seen);
    AssertKinds(seen, {Kind::kPressed, Kind::kLong, Kind::kReleased});
}

void test_keys_a_finger_down_at_boot_is_no_press(void) {
    KeyTracker k(7);
    k.Reset(true, 0);
    TEST_ASSERT_TRUE(k.Pressed());
    Seen seen;
    Run(k, true, 0, 2000, seen);       // held well past kLongMs: no long
    Run(k, false, 2000, 3000, seen);   // the release shows, no click
    AssertKinds(seen, {Kind::kReleased});
    Run(k, true, 3000, 3100, seen);    // the next press counts
    Run(k, false, 3100, 4000, seen);
    AssertKinds(seen, {Kind::kReleased, Kind::kPressed, Kind::kReleased, Kind::kClick});
}

void test_keys_held_ms(void) {
    KeyTracker k(7);
    k.Reset(false, 0);
    TEST_ASSERT_EQUAL_UINT32(0, k.HeldMs(10));
    Seen seen;
    Run(k, true, 0, 100, seen);  // debounced at 25
    TEST_ASSERT_EQUAL_UINT32(75, k.HeldMs(100));
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
    RUN_TEST(test_keys_single_click);
    RUN_TEST(test_keys_double_click);
    RUN_TEST(test_keys_long_press_fires_while_held_and_is_no_click);
    RUN_TEST(test_keys_a_press_of_exactly_the_threshold_is_long_only);
    RUN_TEST(test_keys_a_finger_down_at_boot_is_no_press);
    RUN_TEST(test_keys_held_ms);
}
