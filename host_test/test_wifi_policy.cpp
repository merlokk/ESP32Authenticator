// wifi_policy: the client walk, the AP window, AP+client. Ported from
// approver-esp32 (AP+client tests are ours).

#include "unity.h"
#include "wifi_policy.h"

using wifimgr::Action;
using wifimgr::Desired;
using wifimgr::Failure;
using wifimgr::Policy;
using wifimgr::Settings;
using wifimgr::State;

namespace {

Settings TestSettings(uint8_t network_count, uint8_t rounds = 2) {
    Settings settings;
    settings.network_count = network_count;
    settings.rounds_before_ap = rounds;
    settings.connect_timeout_ms = 500;
    settings.attempt_gap_ms = 100;
    settings.round_backoff_ms = 200;
    settings.max_backoff_ms = 600;
    settings.ap_window_ms = 1000;
    return settings;
}

constexpr uint32_t kStep = 5;

Action RunUntilAction(Policy &policy, uint32_t &now, uint32_t limit_ms) {
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

void StartClient(Policy &policy, uint32_t &now, uint8_t count, uint8_t rounds = 2) {
    policy.Configure(TestSettings(count, rounds), now);
    policy.SetDesired(Desired::kClient, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kConnecting),
                          static_cast<int>(policy.GetState()));
}

void test_starts_off_and_asks_for_nothing(void) {
    Policy policy;
    uint32_t now = 1000;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOff), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone), static_cast<int>(RunUntilAction(policy, now, 5000)));
}

void test_switching_off_stops_the_radio_once(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);

    policy.SetDesired(Desired::kOff, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStop), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOff), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 5000)));
}

void test_desired_ap_goes_up_and_stays_up(void) {
    Policy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(2), now);
    policy.SetDesired(Desired::kAp, now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kAp), static_cast<int>(policy.GetState()));

    policy.OnApClients(0, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 10000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kAp), static_cast<int>(policy.GetState()));
}

void test_asking_for_what_is_already_running_changes_nothing(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);
    policy.OnConnected(now);

    policy.SetDesired(Desired::kClient, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOnline), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 5000)));
}

void test_the_first_attempt_is_the_first_network(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3);
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
}

void test_a_failure_moves_to_the_next_network_after_a_gap(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3);

    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kWaiting), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::kNotFound),
                          static_cast<int>(policy.LastFailure()));

    const uint32_t started = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 1000)));
    TEST_ASSERT_TRUE(now - started >= 100);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
}

void test_the_end_of_the_list_wraps_into_a_new_round(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2, /*rounds=*/3);

    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 1000);  // network 1
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());

    policy.OnFailed(Failure::kNotFound, now);
    const uint32_t started = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 2000)));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(1, policy.Round());
    TEST_ASSERT_TRUE(now - started >= 200);
}

void test_the_backoff_grows_with_the_round_and_is_capped(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/250);

    for (uint32_t round = 1; round <= 5; ++round) {
        policy.OnFailed(Failure::kNotFound, now);
        const uint32_t started = now;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                              static_cast<int>(RunUntilAction(policy, now, 5000)));
        const uint32_t waited = now - started;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(round), policy.Round());

        const uint32_t expected = round * 200 > 600 ? 600 : round * 200;
        TEST_ASSERT_UINT32_WITHIN(2 * kStep, expected, waited);
    }
}

void test_the_ap_goes_up_after_the_configured_rounds(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2, /*rounds=*/2);

    for (int attempt = 0; attempt < 3; ++attempt) {
        policy.OnFailed(Failure::kNotFound, now);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                              static_cast<int>(RunUntilAction(policy, now, 2000)));
    }
    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_UINT8(2, policy.Round());
}

void test_no_rounds_configured_still_tries_every_network_once(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2, /*rounds=*/0);
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());

    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 2000)));
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());

    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp),
                          static_cast<int>(RunUntilAction(policy, now, 2000)));
}

void test_the_network_that_worked_is_tried_first_after_the_radio_comes_back(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3);

    for (int i = 0; i < 2; ++i) {
        policy.OnFailed(Failure::kNotFound, now);
        RunUntilAction(policy, now, 1000);
    }
    TEST_ASSERT_EQUAL_UINT8(2, policy.Network());
    policy.OnConnected(now);

    policy.SetDesired(Desired::kOff, now);
    policy.Tick(now);
    policy.SetDesired(Desired::kClient, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_UINT8(2, policy.Network());
}

void test_the_window_expires_and_the_client_starts_over(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/1);
    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp),
                          static_cast<int>(RunUntilAction(policy, now, 2000)));

    const uint32_t raised = now;
    TEST_ASSERT_TRUE(policy.ApWindowRemainingMs(now) <= 1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_TRUE(now - raised >= 1000);
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
}

void test_a_station_holds_the_window_open(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/1);
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 2000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));

    policy.OnApClients(1, now);
    TEST_ASSERT_EQUAL_UINT32(wifimgr::kHeldOpen, policy.ApWindowRemainingMs(now));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 10000)));
}

void test_being_told_nobody_is_attached_does_not_hold_the_window_open(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/1);
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 2000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));

    const uint32_t raised = now;
    Action action = Action::kNone;
    while (action == Action::kNone && now - raised < 3000) {
        policy.OnApClients(0, now);
        action = policy.Tick(now);
        now += kStep;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient), static_cast<int>(action));
    TEST_ASSERT_TRUE(now - raised >= 1000);
}

void test_the_window_restarts_when_the_last_station_leaves(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/1);
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 2000);

    policy.OnApClients(1, now);
    now += 5000;
    policy.OnApClients(0, now);

    TEST_ASSERT_TRUE(policy.ApWindowRemainingMs(now) > 900);
    const uint32_t left = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_TRUE(now - left >= 1000);
}

void test_an_attempt_that_gets_no_answer_times_out(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);

    const uint32_t started = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_TRUE(now - started >= 500);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Failure::kOther),
                          static_cast<int>(policy.LastFailure()));
}

void test_connecting_stops_every_timer(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);
    policy.OnConnected(now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOnline), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 60000)));
}

void test_a_drop_retries_the_network_it_was_on(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3);

    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    policy.OnConnected(now);
    policy.OnFailed(Failure::kOther, now);

    const uint32_t dropped = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kWaiting), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 1000)));
    TEST_ASSERT_TRUE(now - dropped >= 100);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
}

void test_a_drop_while_online_is_not_an_auth_failure(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);
    policy.OnConnected(now);

    policy.OnFailed(Failure::kAuth, now);
    TEST_ASSERT_FALSE(policy.AuthFailed(0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 1000)));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
}

void test_an_auth_failure_is_sticky_and_the_network_is_skipped(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 3, /*rounds=*/250);

    policy.OnFailed(Failure::kAuth, now);
    TEST_ASSERT_TRUE(policy.AuthFailed(0));
    TEST_ASSERT_FALSE(policy.AuthFailed(1));

    RunUntilAction(policy, now, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 1000);
    TEST_ASSERT_EQUAL_UINT8(2, policy.Network());

    policy.OnFailed(Failure::kNotFound, now);
    RunUntilAction(policy, now, 2000);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Round());
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
}

void test_every_password_refused_goes_straight_to_the_ap(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2, /*rounds=*/10);

    policy.OnFailed(Failure::kAuth, now);
    RunUntilAction(policy, now, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
    policy.OnFailed(Failure::kAuth, now);

    TEST_ASSERT_TRUE(policy.NoCandidates());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));
}

void test_the_ap_window_clears_the_auth_failures(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 1, /*rounds=*/10);
    policy.OnFailed(Failure::kAuth, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_FALSE(policy.AuthFailed(0));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
}

void test_no_networks_is_an_access_point_that_never_expires(void) {
    Policy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(0), now);
    policy.SetDesired(Desired::kClient, now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kApWindow), static_cast<int>(policy.GetState()));
    TEST_ASSERT_TRUE(policy.NoCandidates());
    TEST_ASSERT_EQUAL_UINT32(wifimgr::kHeldOpen, policy.ApWindowRemainingMs(now));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 20000)));
}

void test_the_desired_mode_takes_effect_mid_attempt(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);

    policy.SetDesired(Desired::kAp, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kAp), static_cast<int>(policy.GetState()));

    policy.OnFailed(Failure::kNotFound, now);
    policy.OnConnected(now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kAp), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 5000)));
}

void test_reconfiguring_restarts_and_forgets_the_refusals(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2, /*rounds=*/10);
    policy.OnFailed(Failure::kAuth, now);
    TEST_ASSERT_TRUE(policy.AuthFailed(0));

    policy.Configure(TestSettings(3, 10), now);
    TEST_ASSERT_FALSE(policy.AuthFailed(0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
}

void test_the_millisecond_counter_wrapping_is_just_a_subtraction(void) {
    Policy policy;
    uint32_t now = 0xFFFFFFC0u;
    StartClient(policy, now, 2);
    policy.OnFailed(Failure::kNotFound, now);

    const uint32_t started = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 1000)));
    TEST_ASSERT_TRUE(now - started >= 100);
    TEST_ASSERT_EQUAL_UINT8(1, policy.Network());
}

// --- AP+client ---------------------------------------------------------------

void StartApSta(Policy &policy, uint32_t &now, uint8_t count, uint8_t rounds = 2) {
    policy.Configure(TestSettings(count, rounds), now);
    policy.SetDesired(Desired::kApSta, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
}

void test_apsta_raises_the_ap_then_joins(void) {
    Policy policy;
    uint32_t now = 1000;
    StartApSta(policy, now, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 100)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kConnecting), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());

    policy.OnConnected(now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kOnline), static_cast<int>(policy.GetState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 10000)));
}

void test_apsta_rests_instead_of_a_window_and_never_raises_the_ap_again(void) {
    Policy policy;
    uint32_t now = 1000;
    StartApSta(policy, now, 1, /*rounds=*/2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 100)));
    policy.OnFailed(Failure::kNotFound, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 2000)));
    policy.OnFailed(Failure::kNotFound, now);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kWaiting), static_cast<int>(policy.GetState()));
    TEST_ASSERT_TRUE(policy.Resting());
    const uint32_t rested = now;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_TRUE(now - rested >= 1000);
    TEST_ASSERT_FALSE(policy.Resting());
    TEST_ASSERT_EQUAL_UINT8(0, policy.Round());
}

void test_apsta_with_every_password_refused_rests_and_clears_them(void) {
    Policy policy;
    uint32_t now = 1000;
    StartApSta(policy, now, 1, /*rounds=*/10);
    RunUntilAction(policy, now, 100);
    policy.OnFailed(Failure::kAuth, now);
    TEST_ASSERT_TRUE(policy.AuthFailed(0));
    TEST_ASSERT_TRUE(policy.Resting());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 3000)));
    TEST_ASSERT_FALSE(policy.AuthFailed(0));
}

void test_apsta_with_no_networks_is_only_the_ap(void) {
    Policy policy;
    uint32_t now = 1000;
    StartApSta(policy, now, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone),
                          static_cast<int>(RunUntilAction(policy, now, 20000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kIdle), static_cast<int>(policy.GetState()));

    policy.Configure(TestSettings(1), now);  // a network was added
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 100)));
}

void test_a_drop_in_apsta_rejoins_without_touching_the_ap(void) {
    Policy policy;
    uint32_t now = 1000;
    StartApSta(policy, now, 2);
    RunUntilAction(policy, now, 100);
    policy.OnConnected(now);
    policy.OnFailed(Failure::kOther, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 1000)));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());
}

void test_client_to_apsta_and_back(void) {
    Policy policy;
    uint32_t now = 1000;
    StartClient(policy, now, 2);
    policy.OnConnected(now);

    policy.SetDesired(Desired::kApSta, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient),
                          static_cast<int>(RunUntilAction(policy, now, 100)));
    TEST_ASSERT_EQUAL_UINT8(0, policy.Network());  // the last good one

    policy.SetDesired(Desired::kClient, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient), static_cast<int>(policy.Tick(now)));
}

void test_reconfiguring_the_ap_mode_raises_it_again(void) {
    Policy policy;
    uint32_t now = 1000;
    policy.Configure(TestSettings(1), now);
    policy.SetDesired(Desired::kAp, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    policy.Configure(TestSettings(1), now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartAp), static_cast<int>(policy.Tick(now)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::kAp), static_cast<int>(policy.GetState()));
}

void test_a_pending_action_is_not_overwritten_by_a_timer(void) {
    Policy policy;
    uint32_t now = 1000;
    StartApSta(policy, now, 1);
    // The walk's zero wait expired at once, but the AP had to go out first:
    // the next tick, not the same one, asks for the client.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kStartClient), static_cast<int>(policy.Tick(now)));
}

}  // namespace

void RegisterWifiPolicyTests(void) {
    RUN_TEST(test_starts_off_and_asks_for_nothing);
    RUN_TEST(test_switching_off_stops_the_radio_once);
    RUN_TEST(test_desired_ap_goes_up_and_stays_up);
    RUN_TEST(test_asking_for_what_is_already_running_changes_nothing);
    RUN_TEST(test_the_first_attempt_is_the_first_network);
    RUN_TEST(test_a_failure_moves_to_the_next_network_after_a_gap);
    RUN_TEST(test_the_end_of_the_list_wraps_into_a_new_round);
    RUN_TEST(test_the_backoff_grows_with_the_round_and_is_capped);
    RUN_TEST(test_the_ap_goes_up_after_the_configured_rounds);
    RUN_TEST(test_no_rounds_configured_still_tries_every_network_once);
    RUN_TEST(test_the_network_that_worked_is_tried_first_after_the_radio_comes_back);
    RUN_TEST(test_the_window_expires_and_the_client_starts_over);
    RUN_TEST(test_a_station_holds_the_window_open);
    RUN_TEST(test_being_told_nobody_is_attached_does_not_hold_the_window_open);
    RUN_TEST(test_the_window_restarts_when_the_last_station_leaves);
    RUN_TEST(test_an_attempt_that_gets_no_answer_times_out);
    RUN_TEST(test_connecting_stops_every_timer);
    RUN_TEST(test_a_drop_retries_the_network_it_was_on);
    RUN_TEST(test_a_drop_while_online_is_not_an_auth_failure);
    RUN_TEST(test_an_auth_failure_is_sticky_and_the_network_is_skipped);
    RUN_TEST(test_every_password_refused_goes_straight_to_the_ap);
    RUN_TEST(test_the_ap_window_clears_the_auth_failures);
    RUN_TEST(test_no_networks_is_an_access_point_that_never_expires);
    RUN_TEST(test_the_desired_mode_takes_effect_mid_attempt);
    RUN_TEST(test_reconfiguring_restarts_and_forgets_the_refusals);
    RUN_TEST(test_the_millisecond_counter_wrapping_is_just_a_subtraction);
    RUN_TEST(test_apsta_raises_the_ap_then_joins);
    RUN_TEST(test_apsta_rests_instead_of_a_window_and_never_raises_the_ap_again);
    RUN_TEST(test_apsta_with_every_password_refused_rests_and_clears_them);
    RUN_TEST(test_apsta_with_no_networks_is_only_the_ap);
    RUN_TEST(test_a_drop_in_apsta_rejoins_without_touching_the_ap);
    RUN_TEST(test_client_to_apsta_and_back);
    RUN_TEST(test_reconfiguring_the_ap_mode_raises_it_again);
    RUN_TEST(test_a_pending_action_is_not_overwritten_by_a_timer);
}
