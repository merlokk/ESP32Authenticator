// sync_policy: when to ask an SNTP server. Ported from approver-esp32.

#include "sync_policy.h"
#include "unity.h"

using timesync::Action;
using timesync::kNever;
using timesync::SyncPolicy;
using timesync::SyncSettings;

namespace {

// The six hours of §10.8.2, shrunk so a test is not a wait. The ratios are
// what matter and they are kept: the retry is far shorter than the interval,
// the cap is between the two, and the flap guard is shorter than all of them.
constexpr uint32_t kInterval = 60000;
constexpr uint32_t kGap = 5000;
constexpr uint32_t kRetry = 1000;
constexpr uint32_t kRetryMax = 8000;

SyncSettings TestSettings() {
    SyncSettings settings;
    settings.enabled = true;
    settings.interval_ms = kInterval;
    settings.min_gap_ms = kGap;
    settings.retry_ms = kRetry;
    settings.retry_max_ms = kRetryMax;
    return settings;
}

constexpr uint32_t kStep = 25;

// Pump until it asks for a sync, or until `limit_ms` of pretend time has gone
// by without one.
Action RunUntilSync(SyncPolicy &policy, uint32_t &now, uint32_t limit_ms) {
    const uint32_t deadline = now + limit_ms;
    while (static_cast<int32_t>(deadline - now) >= 0) {
        const Action action = policy.Tick(now);
        if (action != Action::kNone) {
            return action;
        }
        now += kStep;
    }
    return Action::kNone;
}

void AssertSyncs(SyncPolicy &policy, uint32_t &now, uint32_t within_ms) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kSync),
                          static_cast<int>(RunUntilSync(policy, now, within_ms)));
}

void AssertQuiet(SyncPolicy &policy, uint32_t &now, uint32_t for_ms) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilSync(policy, now, for_ms)));
}

// A device that has just come up, joined a network and synced once — where
// most of these tests want to start.
void Synced(SyncPolicy &policy, uint32_t &now) {
    policy.Configure(TestSettings(), now);
    policy.OnInternet(true, now);
    AssertSyncs(policy, now, kInterval);
    policy.OnResult(true, now);
    TEST_ASSERT_TRUE(policy.EverSynced());
}

// --- Nothing to do ----------------------------------------------------------

void test_sync_asks_nothing_with_no_internet(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(), now);

    // Never synced, so it is due — and still asks nothing, because there is
    // nothing to ask. Due is not the same as possible.
    AssertQuiet(policy, now, 10 * kInterval);
    TEST_ASSERT_FALSE(policy.EverSynced());
    TEST_ASSERT_EQUAL_UINT32(kNever, policy.NextSyncInMs(now));
    TEST_ASSERT_EQUAL_UINT32(kNever, policy.SinceLastSyncMs(now));
}

void test_sync_asks_nothing_when_switched_off(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    SyncSettings settings = TestSettings();
    settings.enabled = false;
    policy.Configure(settings, now);
    policy.OnInternet(true, now);

    AssertQuiet(policy, now, 10 * kInterval);
    TEST_ASSERT_EQUAL_UINT32(kNever, policy.NextSyncInMs(now));

    // And `date sync` does not overrule the switch: forcing a sync on a device
    // configured not to sync would be a command that ignores its own setting.
    policy.SyncNow(now);
    AssertQuiet(policy, now, kInterval);
}

void test_sync_asks_nothing_while_one_is_outstanding(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(), now);
    policy.OnInternet(true, now);
    AssertSyncs(policy, now, kInterval);
    TEST_ASSERT_TRUE(policy.Syncing());

    // One exchange at a time. A second would be a second socket against the
    // one session the glue keeps.
    AssertQuiet(policy, now, 4 * kInterval);
    TEST_ASSERT_EQUAL_UINT32(0, policy.NextSyncInMs(now));
}

// --- Boot, and the internet appearing ---------------------------------------

void test_sync_happens_as_soon_as_the_internet_appears(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(), now);
    policy.OnInternet(true, now);

    // Immediately: a device that has never synced is due, which is how "after
    // a reboot" and "when the internet appears" are the same rule.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kSync), static_cast<int>(policy.Tick(now)));

    policy.OnResult(true, now);
    TEST_ASSERT_EQUAL_UINT32(0, policy.SinceLastSyncMs(now));
    TEST_ASSERT_EQUAL_UINT16(1, policy.Successes());
    TEST_ASSERT_EQUAL_UINT16(0, policy.FailuresInARow());
}

void test_sync_waits_out_the_interval_after_a_success(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    Synced(policy, now);

    const uint32_t after_sync = now;
    AssertQuiet(policy, now, kInterval - 2 * kStep);
    AssertSyncs(policy, now, 2 * kInterval);

    // The schedule runs from the answer, not from the attempt, and it is the
    // interval rather than anything shorter.
    TEST_ASSERT_TRUE(now - after_sync >= kInterval);
    TEST_ASSERT_TRUE(now - after_sync < kInterval + 4 * kStep);
}

void test_sync_does_not_repeat_for_every_flap(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    Synced(policy, now);

    // Four reconnections inside the guard. Each one is a reason to *consider*
    // syncing and none of them is a reason to do it: the clock was set a
    // moment ago, and a link bouncing is not new information about the time.
    for (int i = 0; i < 4; ++i) {
        now += kGap / 16;
        policy.OnInternet(false, now);
        now += kGap / 16;
        policy.OnInternet(true, now);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone), static_cast<int>(policy.Tick(now)));
    }
    TEST_ASSERT_EQUAL_UINT16(1, policy.Successes());

    // Past the guard, the next reconnection is worth acting on again — long
    // enough off the air and the drift is real.
    now += kGap;
    policy.OnInternet(false, now);
    policy.OnInternet(true, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kSync), static_cast<int>(policy.Tick(now)));
}

void test_sync_after_a_long_outage_does_not_wait_for_the_interval(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    Synced(policy, now);

    // Off the air for well under the interval but well over the guard. The
    // link coming back is the moment to ask — waiting out the remaining hours
    // on a clock that has been running free is the wrong way round.
    policy.OnInternet(false, now);
    now += kInterval / 2;
    policy.OnInternet(true, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kSync), static_cast<int>(policy.Tick(now)));
}

void test_sync_now_ignores_the_guard(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    Synced(policy, now);

    // The console asked. The guard exists to stop a flapping link from
    // hammering somebody's server, not to argue with an operator.
    now += kGap / 4;
    policy.SyncNow(now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kSync), static_cast<int>(policy.Tick(now)));
}

// --- Failure ----------------------------------------------------------------

void test_sync_retries_sooner_than_the_interval(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(), now);
    policy.OnInternet(true, now);
    AssertSyncs(policy, now, kInterval);

    const uint32_t failed_at = now;
    policy.OnResult(false, now);
    TEST_ASSERT_EQUAL_UINT16(1, policy.FailuresInARow());
    TEST_ASSERT_FALSE(policy.EverSynced());

    AssertSyncs(policy, now, kInterval);
    TEST_ASSERT_TRUE(now - failed_at >= kRetry);
    // Far sooner: six hours is the gap between good answers, not a penalty for
    // a server that was busy.
    TEST_ASSERT_TRUE(now - failed_at < kInterval / 2);
}

void test_sync_backoff_grows_and_is_capped(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(), now);
    policy.OnInternet(true, now);

    uint32_t previous = 0;
    uint32_t longest = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        AssertSyncs(policy, now, 4 * kRetryMax);
        const uint32_t asked_at = now;
        policy.OnResult(false, now);
        AssertSyncs(policy, now, 4 * kRetryMax);
        const uint32_t waited = now - asked_at;

        // **Both halves, because either alone is satisfied by a constant**:
        // it grows while it is under the cap, and it never passes the cap.
        if (attempt > 0 && previous < kRetryMax) {
            TEST_ASSERT_TRUE(waited > previous);
        }
        TEST_ASSERT_TRUE(waited <= kRetryMax + 4 * kStep);
        if (waited > longest) {
            longest = waited;
        }
        previous = waited;
        policy.OnResult(false, now);
    }
    TEST_ASSERT_TRUE(longest > kRetry);
    TEST_ASSERT_EQUAL_UINT16(16, policy.FailuresInARow());
}

void test_sync_success_clears_the_backoff(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(), now);
    policy.OnInternet(true, now);

    for (int attempt = 0; attempt < 4; ++attempt) {
        AssertSyncs(policy, now, 4 * kRetryMax);
        policy.OnResult(false, now);
    }
    AssertSyncs(policy, now, 4 * kRetryMax);
    policy.OnResult(true, now);
    TEST_ASSERT_EQUAL_UINT16(0, policy.FailuresInARow());

    // The next failure starts at the base wait again rather than resuming the
    // grown one: the server answered, so what was learned about it is stale.
    now += kInterval;
    AssertSyncs(policy, now, 2 * kInterval);
    const uint32_t failed_at = now;
    policy.OnResult(false, now);
    AssertSyncs(policy, now, 4 * kRetryMax);
    TEST_ASSERT_TRUE(now - failed_at < 2 * kRetry);
}

void test_sync_ignores_a_result_nobody_asked_for(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    Synced(policy, now);

    const uint32_t synced_at = now;
    now += kInterval / 2;
    // A stray answer must not reschedule anything, or a machine that has moved
    // on gets its next sync pushed out by a packet it is not waiting for.
    policy.OnResult(true, now);
    TEST_ASSERT_EQUAL_UINT16(1, policy.Successes());
    TEST_ASSERT_EQUAL_UINT32(now - synced_at, policy.SinceLastSyncMs(now));
    AssertSyncs(policy, now, kInterval);
    TEST_ASSERT_TRUE(now - synced_at < kInterval + 4 * kStep);
}

// --- Reconfiguring, and the clock underneath it ------------------------------

void test_sync_configure_keeps_what_already_happened(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    Synced(policy, now);

    // A shorter interval typed into the console. It reschedules; it does not
    // un-sync the device — the clock really was set a moment ago, and pinning
    // that is what stops `config set sync` from being a way to hammer a server.
    SyncSettings settings = TestSettings();
    settings.interval_ms = kInterval / 4;
    policy.Configure(settings, now);
    TEST_ASSERT_TRUE(policy.EverSynced());
    TEST_ASSERT_EQUAL_UINT16(1, policy.Successes());

    AssertQuiet(policy, now, kInterval / 4 - 2 * kStep);
    AssertSyncs(policy, now, kInterval);
}

void test_sync_reports_what_a_screen_needs(void) {
    SyncPolicy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(), now);

    // Before anything: never synced, and nothing scheduled because there is no
    // internet to schedule against.
    TEST_ASSERT_EQUAL_UINT32(kNever, policy.SinceLastSyncMs(now));
    TEST_ASSERT_EQUAL_UINT32(kNever, policy.NextSyncInMs(now));

    policy.OnInternet(true, now);
    TEST_ASSERT_EQUAL_UINT32(0, policy.NextSyncInMs(now));
    AssertSyncs(policy, now, kInterval);
    policy.OnResult(true, now);

    now += kInterval / 4;
    TEST_ASSERT_EQUAL_UINT32(kInterval / 4, policy.SinceLastSyncMs(now));
    TEST_ASSERT_EQUAL_UINT32(kInterval - kInterval / 4, policy.NextSyncInMs(now));

    // The link going away does not un-sync the clock; it only means there is
    // nothing to schedule against.
    policy.OnInternet(false, now);
    TEST_ASSERT_EQUAL_UINT32(kInterval / 4, policy.SinceLastSyncMs(now));
    TEST_ASSERT_EQUAL_UINT32(kNever, policy.NextSyncInMs(now));
}

void test_sync_survives_the_millisecond_wrap(void) {
    SyncPolicy policy;
    // `esp_timer` milliseconds wrap every ~49 days, and a desk object is
    // expected to be up for longer than that. Land an interval across the
    // wrap: every deadline here is a signed difference for this reason.
    uint32_t now = 0xFFFFFFFFu - kInterval / 2;
    Synced(policy, now);

    const uint32_t synced_at = now;
    AssertQuiet(policy, now, kInterval - 2 * kStep);
    AssertSyncs(policy, now, kInterval);
    TEST_ASSERT_TRUE(now - synced_at >= kInterval);
    TEST_ASSERT_TRUE(now < synced_at);  // the wrap really did happen
}

}  // namespace

void RegisterTimesyncTests(void) {
    RUN_TEST(test_sync_asks_nothing_with_no_internet);
    RUN_TEST(test_sync_asks_nothing_when_switched_off);
    RUN_TEST(test_sync_asks_nothing_while_one_is_outstanding);
    RUN_TEST(test_sync_happens_as_soon_as_the_internet_appears);
    RUN_TEST(test_sync_waits_out_the_interval_after_a_success);
    RUN_TEST(test_sync_does_not_repeat_for_every_flap);
    RUN_TEST(test_sync_after_a_long_outage_does_not_wait_for_the_interval);
    RUN_TEST(test_sync_now_ignores_the_guard);
    RUN_TEST(test_sync_retries_sooner_than_the_interval);
    RUN_TEST(test_sync_backoff_grows_and_is_capped);
    RUN_TEST(test_sync_success_clears_the_backoff);
    RUN_TEST(test_sync_ignores_a_result_nobody_asked_for);
    RUN_TEST(test_sync_configure_keeps_what_already_happened);
    RUN_TEST(test_sync_reports_what_a_screen_needs);
    RUN_TEST(test_sync_survives_the_millisecond_wrap);
}
