#pragma once

// Which network the radio should be on, and when (ported from approver-esp32).
// Includes nothing but <cstdint>: the decisions run in the host tests; the
// manager carries them out on wifi::Radio.
//
// Client: try each network in turn (last good first); after `rounds_before_ap`
// fruitless passes raise a temporary AP for `ap_window_ms` so someone can
// reach the device and fix the list; nobody came: try again. A station on the
// AP holds the window open. An empty list, or every network refusing its
// password, raises the AP at once.
// AP: the access point, permanently.
// AP+client: the access point permanently, and the client walk beside it;
// instead of the AP window it rests for `ap_window_ms` between cycles.
//
// A wrong password is sticky (that network is skipped) until the AP window or
// rest ends or the list is edited. Every failure passes through a wait: never
// a tight loop. Times are uint32_t ms, only ever subtracted (wrap-safe).

#include <cstdint>

namespace wifimgr {

// Must match config::kMaxNetworks (static_assert in wifi_manager.cpp).
constexpr uint8_t kMaxNetworks = 4;
constexpr uint8_t kNoNetwork = 0xFF;
constexpr uint32_t kHeldOpen = 0xFFFFFFFFu;  // AP window with no end

// What was asked for.
enum class Desired : uint8_t {
    kOff,
    kClient,
    kAp,
    kApSta,
};

// What is happening.
enum class State : uint8_t {
    kOff,
    kIdle,        // AP+client with no networks: only the AP
    kConnecting,  // an attempt on Network() is in flight
    kWaiting,     // between attempts (or resting, AP+client)
    kOnline,      // joined Network()
    kApWindow,    // client mode's temporary AP: nothing would have us
    kAp,          // AP because that was asked for
};

enum class Failure : uint8_t {
    kNone,
    kAuth,
    kNotFound,
    kOther,
};

// What to do to the radio now; one per Tick.
enum class Action : uint8_t {
    kNone,
    kStop,
    kStartAp,
    kStartClient,  // join Network(); keep the AP in AP+client mode
};

struct Settings {
    uint8_t network_count = 0;  // the policy never sees an SSID
    uint8_t rounds_before_ap = 2;  // 0 behaves as 1
    uint32_t connect_timeout_ms = 15000;
    uint32_t attempt_gap_ms = 1000;     // between networks in one round
    uint32_t round_backoff_ms = 5000;   // between rounds, x round, capped
    uint32_t max_backoff_ms = 60000;
    uint32_t ap_window_ms = 120000;  // fallback AP with nobody on it / AP+client rest
};

class Policy {
   public:
    Policy() = default;
    Policy(const Policy &) = delete;
    Policy &operator=(const Policy &) = delete;

    // New settings (the list was edited): clears sticky auth failures and the
    // last good network, and restarts the current mode.
    void Configure(const Settings &settings, uint32_t now_ms);

    // Idempotent: the same value restarts nothing.
    void SetDesired(Desired desired, uint32_t now_ms);
    Desired GetDesired() const { return desired_; }

    // What the radio reports.
    void OnConnected(uint32_t now_ms);
    void OnFailed(Failure why, uint32_t now_ms);
    // Stations on our AP; only the falling edge to 0 restarts the window.
    void OnApClients(uint8_t count, uint32_t now_ms);

    // Runs the timers and returns the one thing to do, if any.
    Action Tick(uint32_t now_ms);

    State GetState() const { return state_; }
    uint8_t Network() const { return network_; }  // kNoNetwork when none
    uint8_t Round() const { return round_; }      // 0-based pass over the list
    Failure LastFailure() const { return failure_; }
    bool AuthFailed(uint8_t index) const;
    bool Resting() const { return resting_; }
    bool NoCandidates() const;

    // Meaningful in kApWindow / kWaiting respectively.
    uint32_t ApWindowRemainingMs(uint32_t now_ms) const;
    uint32_t WaitRemainingMs(uint32_t now_ms) const;

   private:
    void Enter(uint32_t now_ms);
    void EnterWaiting(uint32_t delay_ms, uint32_t now_ms);
    void EnterApWindow(uint32_t now_ms);
    void EnterRest(uint32_t now_ms);
    void GiveUp(uint32_t now_ms);
    void BeginAttempt(uint32_t now_ms);
    void RestartClient(uint32_t now_ms);
    void AdvanceAfterFailure(uint32_t now_ms);
    uint8_t NextCandidateFrom(uint8_t start) const;
    uint32_t BackoffForRound(uint8_t round) const;

    Settings settings_ = {};
    Desired desired_ = Desired::kOff;
    State state_ = State::kOff;
    Action pending_ = Action::kNone;

    uint8_t network_ = kNoNetwork;
    uint8_t last_good_ = kNoNetwork;  // tried first
    uint8_t round_ = 0;
    uint8_t auth_failed_ = 0;  // bit per network
    static_assert(kMaxNetworks <= 8, "auth_failed_ is a bitmask in a uint8_t");
    bool resting_ = false;  // the wait is AP+client's rest, not a backoff

    uint8_t ap_clients_ = 0;
    Failure failure_ = Failure::kNone;

    uint32_t attempt_started_ms_ = 0;
    uint32_t wait_started_ms_ = 0;
    uint32_t wait_ms_ = 0;
    uint32_t window_started_ms_ = 0;
};

const char *Name(Desired desired);
const char *Name(State state);
const char *Name(Failure failure);

}  // namespace wifimgr
