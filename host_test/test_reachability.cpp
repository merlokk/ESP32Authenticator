// reachability: when to ping, what a failed round means, online/offline.
// Ported from approver-esp32.

#include "reachability.h"
#include "unity.h"

using wifimgr::Internet;
using wifimgr::Probe;
using wifimgr::ProbeSettings;
using wifimgr::Reachability;

namespace {

ProbeSettings TestSettings(uint8_t targets, uint8_t failures = 2) {
    ProbeSettings settings;
    settings.enabled = true;
    settings.target_count = targets;
    settings.interval_ms = 1000;  // the minute of §10.9, shrunk
    settings.failures_before_offline = failures;
    return settings;
}

constexpr uint32_t kStep = 5;

Probe RunUntilProbe(Reachability &reach, uint32_t &now, uint32_t limit_ms) {
    const uint32_t deadline = now + limit_ms;
    while (static_cast<int32_t>(deadline - now) >= 0) {
        const Probe probe = reach.Tick(now);
        if (probe != Probe::kNone) {
            return probe;
        }
        now += kStep;
    }
    return Probe::kNone;
}

void Online(Reachability &reach, uint32_t &now, uint8_t targets = 3) {
    reach.Configure(TestSettings(targets), now);
    reach.LinkUp(now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend), static_cast<int>(reach.Tick(now)));
    reach.OnResult(true, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kOnline), static_cast<int>(reach.State()));
}

void test_reach_asks_nothing_with_no_link(void) {
    Reachability reach;
    uint32_t now = 1000;
    reach.Configure(TestSettings(3), now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kNone),
                          static_cast<int>(RunUntilProbe(reach, now, 10000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kUnknown), static_cast<int>(reach.State()));
}

void test_reach_asks_nothing_when_switched_off_or_with_no_targets(void) {
    Reachability off;
    uint32_t now = 1000;
    ProbeSettings settings = TestSettings(3);
    settings.enabled = false;
    off.Configure(settings, now);
    off.LinkUp(now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kNone),
                          static_cast<int>(RunUntilProbe(off, now, 10000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kUnknown), static_cast<int>(off.State()));

    Reachability empty;
    now = 1000;
    empty.Configure(TestSettings(0), now);
    empty.LinkUp(now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kNone),
                          static_cast<int>(RunUntilProbe(empty, now, 10000)));
}

void test_reach_asks_the_moment_the_link_comes_up(void) {
    Reachability reach;
    uint32_t now = 1000;
    reach.Configure(TestSettings(3), now);
    reach.LinkUp(now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend), static_cast<int>(reach.Tick(now)));
    TEST_ASSERT_EQUAL_UINT8(0, reach.Target());
    TEST_ASSERT_TRUE(reach.Probing());
}

void test_reach_asks_nothing_more_while_one_is_outstanding(void) {
    Reachability reach;
    uint32_t now = 1000;
    reach.Configure(TestSettings(3), now);
    reach.LinkUp(now);
    reach.Tick(now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kNone),
                          static_cast<int>(RunUntilProbe(reach, now, 10000)));
}

void test_reach_waits_an_interval_between_rounds(void) {
    Reachability reach;
    uint32_t now = 1000;
    Online(reach, now);

    const uint32_t answered = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend),
                          static_cast<int>(RunUntilProbe(reach, now, 5000)));
    TEST_ASSERT_TRUE(now - answered >= 1000);
}

void test_reach_tries_the_next_target_at_once_not_next_minute(void) {
    Reachability reach;
    uint32_t now = 1000;
    reach.Configure(TestSettings(3), now);
    reach.LinkUp(now);
    reach.Tick(now);
    TEST_ASSERT_EQUAL_UINT8(0, reach.Target());

    reach.OnResult(false, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend), static_cast<int>(reach.Tick(now)));
    TEST_ASSERT_EQUAL_UINT8(1, reach.Target());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kUnknown), static_cast<int>(reach.State()));

    reach.OnResult(true, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kOnline), static_cast<int>(reach.State()));
    TEST_ASSERT_EQUAL_UINT8(0, reach.FailedRounds());
}

void test_reach_a_whole_round_failing_is_one_failure(void) {
    Reachability reach;
    uint32_t now = 1000;
    reach.Configure(TestSettings(3), now);
    reach.LinkUp(now);

    reach.Tick(now);
    for (int i = 0; i < 2; ++i) {
        reach.OnResult(false, now);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend), static_cast<int>(reach.Tick(now)));
    }
    reach.OnResult(false, now);  // the third and last target

    TEST_ASSERT_EQUAL_UINT8(1, reach.FailedRounds());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kUnknown), static_cast<int>(reach.State()));
    const uint32_t failed_at = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend),
                          static_cast<int>(RunUntilProbe(reach, now, 5000)));
    TEST_ASSERT_TRUE(now - failed_at >= 1000);
}

void FailARound(Reachability &reach, uint32_t &now, uint8_t targets) {
    for (uint8_t i = 0; i < targets; ++i) {
        if (i > 0) {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend),
                                  static_cast<int>(reach.Tick(now)));
        }
        reach.OnResult(false, now);
    }
}

void test_reach_says_offline_only_after_consecutive_failures(void) {
    Reachability reach;
    uint32_t now = 1000;
    Online(reach, now, 2);

    RunUntilProbe(reach, now, 5000);
    FailARound(reach, now, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kOnline), static_cast<int>(reach.State()));
    TEST_ASSERT_EQUAL_UINT8(1, reach.FailedRounds());

    RunUntilProbe(reach, now, 5000);
    FailARound(reach, now, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kOffline), static_cast<int>(reach.State()));
    TEST_ASSERT_EQUAL_UINT8(2, reach.FailedRounds());
}

void test_reach_one_reply_is_enough_to_be_back(void) {
    Reachability reach;
    uint32_t now = 1000;
    Online(reach, now, 2);
    for (int round = 0; round < 3; ++round) {
        RunUntilProbe(reach, now, 5000);
        FailARound(reach, now, 2);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kOffline), static_cast<int>(reach.State()));

    RunUntilProbe(reach, now, 5000);
    reach.OnResult(true, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kOnline), static_cast<int>(reach.State()));
    TEST_ASSERT_EQUAL_UINT8(0, reach.FailedRounds());
}

void test_reach_remembers_which_target_answered(void) {
    Reachability reach;
    uint32_t now = 1000;
    reach.Configure(TestSettings(3), now);
    reach.LinkUp(now);

    reach.Tick(now);            // target 0
    reach.OnResult(false, now);  // blocked here, and it will be next time too
    reach.Tick(now);            // target 1
    reach.OnResult(true, now);

    RunUntilProbe(reach, now, 5000);
    TEST_ASSERT_EQUAL_UINT8(1, reach.Target());
}

void test_reach_forgets_what_it_knew_when_the_link_drops(void) {
    Reachability reach;
    uint32_t now = 1000;
    Online(reach, now);

    reach.LinkDown(now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kUnknown), static_cast<int>(reach.State()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kNone),
                          static_cast<int>(RunUntilProbe(reach, now, 10000)));
}

void test_reach_ignores_an_answer_that_arrives_after_the_link_went(void) {
    Reachability reach;
    uint32_t now = 1000;
    reach.Configure(TestSettings(3), now);
    reach.LinkUp(now);
    reach.Tick(now);  // a probe is in flight

    reach.LinkDown(now);
    reach.OnResult(true, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kUnknown), static_cast<int>(reach.State()));
}

void test_reach_asks_again_as_soon_as_a_link_comes_back(void) {
    Reachability reach;
    uint32_t now = 1000;
    Online(reach, now);
    reach.LinkDown(now);
    now += 50;
    reach.LinkUp(now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend), static_cast<int>(reach.Tick(now)));
}

void test_reach_probe_now_does_not_wait_for_the_interval(void) {
    Reachability reach;
    uint32_t now = 1000;
    Online(reach, now);

    now += 10;  // nowhere near the interval
    reach.ProbeNow(now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend), static_cast<int>(reach.Tick(now)));
}

void test_reach_reports_how_long_since_anything_answered(void) {
    Reachability reach;
    uint32_t now = 1000;
    reach.Configure(TestSettings(2), now);
    reach.LinkUp(now);

    TEST_ASSERT_EQUAL_UINT32(wifimgr::kNeverSucceeded, reach.SinceLastSuccessMs(now));

    reach.Tick(now);
    reach.OnResult(true, now);
    now += 2500;
    TEST_ASSERT_EQUAL_UINT32(2500, reach.SinceLastSuccessMs(now));
}

void test_reach_survives_the_millisecond_wrap(void) {
    Reachability reach;
    uint32_t now = 0xFFFFFFC0u;
    Online(reach, now);

    const uint32_t answered = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Probe::kSend),
                          static_cast<int>(RunUntilProbe(reach, now, 5000)));
    TEST_ASSERT_TRUE(now - answered >= 1000);
}

void test_reach_reconfiguring_starts_again(void) {
    Reachability reach;
    uint32_t now = 1000;
    Online(reach, now);

    reach.Configure(TestSettings(2), now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Internet::kUnknown), static_cast<int>(reach.State()));
    TEST_ASSERT_EQUAL_UINT8(0, reach.FailedRounds());
}

}  // namespace

void RegisterReachabilityTests(void) {
    RUN_TEST(test_reach_asks_nothing_with_no_link);
    RUN_TEST(test_reach_asks_nothing_when_switched_off_or_with_no_targets);
    RUN_TEST(test_reach_asks_the_moment_the_link_comes_up);
    RUN_TEST(test_reach_asks_nothing_more_while_one_is_outstanding);
    RUN_TEST(test_reach_waits_an_interval_between_rounds);
    RUN_TEST(test_reach_tries_the_next_target_at_once_not_next_minute);
    RUN_TEST(test_reach_a_whole_round_failing_is_one_failure);
    RUN_TEST(test_reach_says_offline_only_after_consecutive_failures);
    RUN_TEST(test_reach_one_reply_is_enough_to_be_back);
    RUN_TEST(test_reach_remembers_which_target_answered);
    RUN_TEST(test_reach_forgets_what_it_knew_when_the_link_drops);
    RUN_TEST(test_reach_ignores_an_answer_that_arrives_after_the_link_went);
    RUN_TEST(test_reach_asks_again_as_soon_as_a_link_comes_back);
    RUN_TEST(test_reach_probe_now_does_not_wait_for_the_interval);
    RUN_TEST(test_reach_reports_how_long_since_anything_answered);
    RUN_TEST(test_reach_survives_the_millisecond_wrap);
    RUN_TEST(test_reach_reconfiguring_starts_again);
}
