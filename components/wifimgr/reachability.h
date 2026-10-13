#pragma once

// Is anything reachable through the link (ported from approver-esp32).
// Associated is not online: a router with no uplink or a captive portal look
// the same from the station. <cstdint> only: host-tested; the manager sends
// the ICMP echo.
//
// - A round starts when the client link comes up, then every interval.
// - A round tries the targets in turn, starting with the last one that
//   answered; one reply ends it (online). A target that drops ICMP is not an
//   outage: only a whole round failing counts, and only
//   `failures_before_offline` consecutive failed rounds mean offline.
// - Unknown while there is no link, the check is off, or the first round is
//   still out.

#include <cstdint>

namespace wifimgr {

// Must match config::kMaxProbeTargets (static_assert in wifi_manager.cpp).
constexpr uint8_t kMaxProbeTargets = 4;
constexpr uint8_t kNoTarget = 0xFF;
constexpr uint32_t kNeverSucceeded = 0xFFFFFFFFu;  // also "no next probe"

enum class Internet : uint8_t {
    kUnknown,
    kOnline,
    kOffline,
};

enum class Probe : uint8_t {
    kNone,
    kSend,  // ping Target(), then call OnResult
};

struct ProbeSettings {
    bool enabled = true;
    uint8_t target_count = 0;  // never an address: the manager keeps those
    uint32_t interval_ms = 60000;
    uint8_t failures_before_offline = 2;
};

class Reachability {
   public:
    Reachability() = default;
    Reachability(const Reachability &) = delete;
    Reachability &operator=(const Reachability &) = delete;

    // New settings: forgets what it knew, asks at once if the link is up.
    void Configure(const ProbeSettings &settings, uint32_t now_ms);

    // A client link with an address came up / went.
    void LinkUp(uint32_t now_ms);
    void LinkDown(uint32_t now_ms);
    bool LinkIsUp() const { return link_up_; }

    // Start a round now instead of at the next interval.
    void ProbeNow(uint32_t now_ms);

    Probe Tick(uint32_t now_ms);
    // The answer to the last kSend; ignored when none is outstanding.
    void OnResult(bool reachable, uint32_t now_ms);

    Internet State() const { return state_; }
    uint8_t Target() const { return target_; }
    uint8_t FailedRounds() const { return failed_rounds_; }
    uint32_t SinceLastSuccessMs(uint32_t now_ms) const;  // kNeverSucceeded if none
    uint32_t NextProbeInMs(uint32_t now_ms) const;       // 0 now, kNeverSucceeded never
    bool Probing() const { return awaiting_result_; }

   private:
    void StartRound(uint32_t now_ms);
    void ScheduleNextRound(uint32_t now_ms);
    uint8_t NthTarget(uint8_t attempt) const;

    ProbeSettings settings_ = {};
    Internet state_ = Internet::kUnknown;
    bool link_up_ = false;

    uint8_t target_ = kNoTarget;
    uint8_t preferred_ = 0;  // the last one that answered
    uint8_t attempts_ = 0;   // targets tried in this round
    uint8_t failed_rounds_ = 0;

    bool awaiting_result_ = false;
    bool round_due_ = false;
    bool have_success_ = false;

    uint32_t wait_started_ms_ = 0;
    uint32_t last_success_ms_ = 0;
};

const char *Name(Internet state);

}  // namespace wifimgr
