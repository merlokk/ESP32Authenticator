#include "sync_policy.h"

namespace timesync {

void SyncPolicy::Configure(const SyncSettings &settings, uint32_t now_ms) {
    settings_ = settings;
    // Floors: an interval of zero would poll somebody else's NTP server.
    if (settings_.interval_ms < 1000u) {
        settings_.interval_ms = 1000u;
    }
    if (settings_.retry_ms < 1000u) {
        settings_.retry_ms = 1000u;
    }
    if (settings_.retry_max_ms < settings_.retry_ms) {
        settings_.retry_max_ms = settings_.retry_ms;
    }
    backoff_ms_ = settings_.retry_ms;
    next_at_ms_ = have_sync_ ? last_sync_ms_ + settings_.interval_ms : now_ms;
}

void SyncPolicy::OnInternet(bool usable, uint32_t now_ms) {
    if (usable == usable_) {
        return;
    }
    usable_ = usable;
    if (!usable_) {
        return;  // an outstanding exchange answers or times out on its own
    }
    if (!have_sync_) {
        next_at_ms_ = now_ms;  // never synced: due (the boot case)
        return;
    }
    if (Reached(now_ms, last_sync_ms_ + settings_.min_gap_ms)) {
        // Earlier, not assigned: an overdue sync must not be pushed out.
        next_at_ms_ = Earlier(next_at_ms_, now_ms);
    }
    // Inside the guard: a bounced link, not news about the time.
}

void SyncPolicy::SyncNow(uint32_t now_ms) { next_at_ms_ = now_ms; }

Action SyncPolicy::Tick(uint32_t now_ms) {
    if (!settings_.enabled || !usable_ || awaiting_ || !Reached(now_ms, next_at_ms_)) {
        return Action::kNone;
    }
    awaiting_ = true;
    return Action::kSync;
}

void SyncPolicy::OnResult(bool ok, uint32_t now_ms) {
    if (!awaiting_) {
        return;
    }
    awaiting_ = false;
    if (ok) {
        have_sync_ = true;
        last_sync_ms_ = now_ms;
        if (successes_ < 0xFFFFu) {
            ++successes_;
        }
        failures_ = 0;
        backoff_ms_ = settings_.retry_ms;
        next_at_ms_ = now_ms + settings_.interval_ms;
        return;
    }
    if (failures_ < 0xFFFFu) {
        ++failures_;
    }
    next_at_ms_ = now_ms + backoff_ms_;
    backoff_ms_ =
        backoff_ms_ > settings_.retry_max_ms / 2u ? settings_.retry_max_ms : backoff_ms_ * 2u;
}

uint32_t SyncPolicy::SinceLastSyncMs(uint32_t now_ms) const {
    return have_sync_ ? now_ms - last_sync_ms_ : kNever;
}

uint32_t SyncPolicy::NextSyncInMs(uint32_t now_ms) const {
    if (!settings_.enabled || !usable_) {
        return kNever;
    }
    if (awaiting_ || Reached(now_ms, next_at_ms_)) {
        return 0;
    }
    return next_at_ms_ - now_ms;
}

}  // namespace timesync
