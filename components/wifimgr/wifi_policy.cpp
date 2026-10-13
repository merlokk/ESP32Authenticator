#include "wifi_policy.h"

namespace wifimgr {

void Policy::Configure(const Settings &settings, uint32_t now_ms) {
    settings_ = settings;
    if (settings_.network_count > kMaxNetworks) {
        settings_.network_count = kMaxNetworks;
    }
    // The list was edited: index 2 may be another network now, and the
    // refused password is the likeliest reason for the edit.
    auth_failed_ = 0;
    last_good_ = kNoNetwork;
    round_ = 0;
    if (desired_ != Desired::kOff) {
        Enter(now_ms);
    }
}

void Policy::SetDesired(Desired desired, uint32_t now_ms) {
    // The manager calls this on every pass: re-asking must not drop a link.
    if (desired == desired_) {
        return;
    }
    desired_ = desired;
    Enter(now_ms);
}

// (Re)starts the current mode from the top.
void Policy::Enter(uint32_t now_ms) {
    resting_ = false;
    switch (desired_) {
        case Desired::kOff:
            state_ = State::kOff;
            network_ = kNoNetwork;
            round_ = 0;
            pending_ = Action::kStop;
            break;
        case Desired::kAp:
            state_ = State::kAp;
            ap_clients_ = 0;
            pending_ = Action::kStartAp;
            break;
        case Desired::kClient:
            RestartClient(now_ms);
            break;
        case Desired::kApSta:
            // The AP first; the walk starts on the next tick (one action each).
            pending_ = Action::kStartAp;
            round_ = 0;
            network_ = last_good_;
            EnterWaiting(0, now_ms);
            break;
    }
}

void Policy::OnConnected(uint32_t now_ms) {
    (void)now_ms;
    // Only an attempt can succeed; a late answer belongs to an old question.
    if (state_ != State::kConnecting) {
        return;
    }
    state_ = State::kOnline;
    failure_ = Failure::kNone;
    last_good_ = network_;
    round_ = 0;
    if (network_ < kMaxNetworks) {
        auth_failed_ = static_cast<uint8_t>(auth_failed_ & ~(1u << network_));
    }
}

void Policy::OnFailed(Failure why, uint32_t now_ms) {
    failure_ = why;
    if (state_ == State::kOnline) {
        // A drop is not a refusal (a marginal link times out the handshake
        // too): back to the same network after the gap, cycle from the top.
        round_ = 0;
        network_ = last_good_;
        EnterWaiting(settings_.attempt_gap_ms, now_ms);
        return;
    }
    if (state_ != State::kConnecting) {
        return;
    }
    if (why == Failure::kAuth && network_ < kMaxNetworks) {
        auth_failed_ = static_cast<uint8_t>(auth_failed_ | (1u << network_));
    }
    AdvanceAfterFailure(now_ms);
}

void Policy::OnApClients(uint8_t count, uint32_t now_ms) {
    const uint8_t before = ap_clients_;
    ap_clients_ = count;
    // The last station left: the window starts again from the beginning.
    if (state_ == State::kApWindow && before > 0 && count == 0) {
        window_started_ms_ = now_ms;
    }
}

Action Policy::Tick(uint32_t now_ms) {
    // An action not yet handed out goes first: a timer must not overwrite it.
    if (pending_ != Action::kNone) {
        const Action action = pending_;
        pending_ = Action::kNone;
        return action;
    }
    switch (state_) {
        case State::kConnecting:
            if (now_ms - attempt_started_ms_ >= settings_.connect_timeout_ms) {
                failure_ = Failure::kOther;  // the driver said nothing at all
                AdvanceAfterFailure(now_ms);
            }
            break;

        case State::kWaiting:
            if (now_ms - wait_started_ms_ >= wait_ms_) {
                if (resting_) {
                    resting_ = false;
                    auth_failed_ = 0;
                    round_ = 0;
                    network_ = kNoNetwork;
                }
                BeginAttempt(now_ms);
            }
            break;

        case State::kApWindow:
            if (ApWindowRemainingMs(now_ms) == 0) {
                // Nobody came: the window was the chance to fix a password.
                auth_failed_ = 0;
                round_ = 0;
                network_ = kNoNetwork;
                BeginAttempt(now_ms);
            }
            break;

        case State::kOff:
        case State::kIdle:
        case State::kOnline:
        case State::kAp:
            break;
    }
    const Action action = pending_;
    pending_ = Action::kNone;
    return action;
}

bool Policy::AuthFailed(uint8_t index) const {
    return index < kMaxNetworks && (auth_failed_ & (1u << index)) != 0;
}

bool Policy::NoCandidates() const { return NextCandidateFrom(0) == kNoNetwork; }

uint32_t Policy::ApWindowRemainingMs(uint32_t now_ms) const {
    // Someone is on it, or there is nothing to go back to.
    if (ap_clients_ > 0 || settings_.network_count == 0) {
        return kHeldOpen;
    }
    const uint32_t elapsed = now_ms - window_started_ms_;
    return elapsed >= settings_.ap_window_ms ? 0 : settings_.ap_window_ms - elapsed;
}

uint32_t Policy::WaitRemainingMs(uint32_t now_ms) const {
    const uint32_t elapsed = now_ms - wait_started_ms_;
    return elapsed >= wait_ms_ ? 0 : wait_ms_ - elapsed;
}

// --- transitions --------------------------------------------------------------

void Policy::EnterWaiting(uint32_t delay_ms, uint32_t now_ms) {
    state_ = State::kWaiting;
    wait_started_ms_ = now_ms;
    wait_ms_ = delay_ms;
}

void Policy::EnterApWindow(uint32_t now_ms) {
    state_ = State::kApWindow;
    window_started_ms_ = now_ms;
    ap_clients_ = 0;
    pending_ = Action::kStartAp;
}

void Policy::EnterRest(uint32_t now_ms) {
    if (settings_.network_count == 0) {
        state_ = State::kIdle;  // nothing to rest for; Configure restarts
        network_ = kNoNetwork;
        return;
    }
    EnterWaiting(settings_.ap_window_ms, now_ms);
    resting_ = true;
}

// The client walk has nowhere to go: findable (client) or rest (AP+client).
void Policy::GiveUp(uint32_t now_ms) {
    if (desired_ == Desired::kApSta) {
        EnterRest(now_ms);
    } else {
        EnterApWindow(now_ms);
    }
}

void Policy::BeginAttempt(uint32_t now_ms) {
    // The index may be stale: the list shrank, or it was struck off.
    if (network_ >= settings_.network_count || AuthFailed(network_)) {
        network_ = NextCandidateFrom(0);
    }
    if (network_ == kNoNetwork) {
        GiveUp(now_ms);
        return;
    }
    state_ = State::kConnecting;
    attempt_started_ms_ = now_ms;
    pending_ = Action::kStartClient;
}

void Policy::RestartClient(uint32_t now_ms) {
    round_ = 0;
    network_ = last_good_;
    BeginAttempt(now_ms);
}

void Policy::AdvanceAfterFailure(uint32_t now_ms) {
    const uint8_t next =
        network_ == kNoNetwork ? kNoNetwork : NextCandidateFrom(static_cast<uint8_t>(network_ + 1));
    if (next != kNoNetwork) {
        network_ = next;
        EnterWaiting(settings_.attempt_gap_ms, now_ms);
        return;
    }
    // A round is over. Counted before compared, so rounds 0 still tries each
    // network once.
    ++round_;
    if (round_ >= settings_.rounds_before_ap || NoCandidates()) {
        GiveUp(now_ms);
        return;
    }
    network_ = NextCandidateFrom(0);
    EnterWaiting(BackoffForRound(round_), now_ms);
}

uint8_t Policy::NextCandidateFrom(uint8_t start) const {
    for (uint8_t i = start; i < settings_.network_count; ++i) {
        if (!AuthFailed(i)) {
            return i;
        }
    }
    return kNoNetwork;
}

uint32_t Policy::BackoffForRound(uint8_t round) const {
    const uint32_t scaled = settings_.round_backoff_ms * round;
    if (round != 0 && scaled / round != settings_.round_backoff_ms) {
        return settings_.max_backoff_ms;  // the multiply wrapped
    }
    return scaled > settings_.max_backoff_ms ? settings_.max_backoff_ms : scaled;
}

// --- names --------------------------------------------------------------------

const char *Name(Desired desired) {
    switch (desired) {
        case Desired::kOff: return "off";
        case Desired::kClient: return "client";
        case Desired::kAp: return "ap";
        case Desired::kApSta: return "apsta";
    }
    return "?";
}

const char *Name(State state) {
    switch (state) {
        case State::kOff: return "off";
        case State::kIdle: return "no networks";
        case State::kConnecting: return "connecting";
        case State::kWaiting: return "waiting";
        case State::kOnline: return "connected";
        case State::kApWindow: return "temporary ap";
        case State::kAp: return "ap";
    }
    return "?";
}

const char *Name(Failure failure) {
    switch (failure) {
        case Failure::kNone: return "none";
        case Failure::kAuth: return "wrong password";
        case Failure::kNotFound: return "no such network";
        case Failure::kOther: return "disconnected";
    }
    return "?";
}

}  // namespace wifimgr
