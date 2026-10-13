#include "reachability.h"

namespace wifimgr {

void Reachability::Configure(const ProbeSettings &settings, uint32_t now_ms) {
    settings_ = settings;
    if (settings_.target_count > kMaxProbeTargets) {
        settings_.target_count = kMaxProbeTargets;
    }
    state_ = Internet::kUnknown;
    preferred_ = 0;
    failed_rounds_ = 0;
    awaiting_result_ = false;
    target_ = kNoTarget;
    attempts_ = 0;
    if (link_up_) {
        ProbeNow(now_ms);
    } else {
        round_due_ = false;
    }
}

void Reachability::LinkUp(uint32_t now_ms) {
    if (link_up_) {
        return;  // reported on every pass: only the edge counts
    }
    link_up_ = true;
    ProbeNow(now_ms);
}

void Reachability::LinkDown(uint32_t now_ms) {
    (void)now_ms;
    link_up_ = false;
    round_due_ = false;
    attempts_ = 0;
    target_ = kNoTarget;
    state_ = Internet::kUnknown;
    failed_rounds_ = 0;
    awaiting_result_ = false;  // an answer still out is about another link
}

void Reachability::ProbeNow(uint32_t now_ms) {
    if (!link_up_) {
        return;
    }
    attempts_ = 0;
    round_due_ = true;
    wait_started_ms_ = now_ms;
}

Probe Reachability::Tick(uint32_t now_ms) {
    if (!settings_.enabled || !link_up_ || settings_.target_count == 0 || awaiting_result_) {
        return Probe::kNone;
    }
    if (!round_due_ && now_ms - wait_started_ms_ >= settings_.interval_ms) {
        StartRound(now_ms);
    }
    if (!round_due_) {
        return Probe::kNone;
    }
    target_ = NthTarget(attempts_);
    if (target_ == kNoTarget) {
        round_due_ = false;
        return Probe::kNone;
    }
    awaiting_result_ = true;
    return Probe::kSend;
}

void Reachability::OnResult(bool reachable, uint32_t now_ms) {
    if (!awaiting_result_) {
        return;
    }
    awaiting_result_ = false;
    if (reachable) {
        preferred_ = target_;
        state_ = Internet::kOnline;
        failed_rounds_ = 0;
        have_success_ = true;
        last_success_ms_ = now_ms;
        ScheduleNextRound(now_ms);
        return;
    }
    // The next target at once (next Tick), not next interval.
    ++attempts_;
    if (attempts_ < settings_.target_count) {
        return;
    }
    if (failed_rounds_ < 0xFF) {
        ++failed_rounds_;
    }
    if (failed_rounds_ >= settings_.failures_before_offline) {
        state_ = Internet::kOffline;
    }
    ScheduleNextRound(now_ms);
}

uint32_t Reachability::SinceLastSuccessMs(uint32_t now_ms) const {
    return have_success_ ? now_ms - last_success_ms_ : kNeverSucceeded;
}

uint32_t Reachability::NextProbeInMs(uint32_t now_ms) const {
    if (!settings_.enabled || !link_up_ || settings_.target_count == 0) {
        return kNeverSucceeded;
    }
    if (round_due_ || awaiting_result_) {
        return 0;
    }
    const uint32_t elapsed = now_ms - wait_started_ms_;
    return elapsed >= settings_.interval_ms ? 0 : settings_.interval_ms - elapsed;
}

void Reachability::StartRound(uint32_t now_ms) {
    attempts_ = 0;
    round_due_ = true;
    wait_started_ms_ = now_ms;
}

void Reachability::ScheduleNextRound(uint32_t now_ms) {
    round_due_ = false;
    attempts_ = 0;
    wait_started_ms_ = now_ms;
}

uint8_t Reachability::NthTarget(uint8_t attempt) const {
    if (attempt >= settings_.target_count) {
        return kNoTarget;
    }
    return static_cast<uint8_t>((preferred_ + attempt) % settings_.target_count);
}

const char *Name(Internet state) {
    switch (state) {
        case Internet::kUnknown: return "unknown";
        case Internet::kOnline: return "online";
        case Internet::kOffline: return "offline";
    }
    return "?";
}

}  // namespace wifimgr
