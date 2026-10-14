#pragma once

// When to ask an SNTP server for the time. Pure (host-tested); the exchange is
// timesync.cpp. Ported from approver-esp32.
//
// At boot, when the internet appears, and every `interval` after that. The
// first two are one rule: a device that never synced is due. Whether there is
// an internet is wifimgr's answer, read here, not a second probe.

#include <cstdint>

namespace timesync {

// "Nothing to age / nothing scheduled". Not zero, which reads as "just now".
inline constexpr uint32_t kNever = 0xFFFFFFFFu;

struct SyncSettings {
    bool enabled = true;
    uint32_t interval_ms = 6u * 60u * 60u * 1000u;  // config.json time.syncHours
    // Flap guard: a link that comes back is a reason to sync, four times a
    // minute is not four reasons. No re-sync within this of a good sync.
    uint32_t min_gap_ms = 5u * 60u * 1000u;
    // A failure retries well before the interval; the wait doubles up to a cap.
    uint32_t retry_ms = 60u * 1000u;
    uint32_t retry_max_ms = 15u * 60u * 1000u;
};

enum class Action : uint8_t {
    kNone,
    kSync,  // run one exchange, then OnResult
};

class SyncPolicy {
public:
    SyncPolicy() = default;
    SyncPolicy(const SyncPolicy &) = delete;
    SyncPolicy &operator=(const SyncPolicy &) = delete;

    // Keeps what happened: a sync twenty minutes ago is still true after an
    // interval change, so this only reschedules.
    void Configure(const SyncSettings &settings, uint32_t now_ms);

    // Is there something to ask through (the manager decides what that means).
    void OnInternet(bool usable, uint32_t now_ms);
    bool InternetIsUsable() const { return usable_; }

    // Ask at the next tick, ignoring the interval and the flap guard
    // (`date sync`). Still needs an internet.
    void SyncNow(uint32_t now_ms);

    // kSync: run one exchange and call OnResult. Nothing while one is out.
    Action Tick(uint32_t now_ms);

    // The answer to the sync Tick asked for; an unasked result is ignored.
    void OnResult(bool ok, uint32_t now_ms);

    bool Enabled() const { return settings_.enabled; }
    bool Syncing() const { return awaiting_; }
    bool EverSynced() const { return have_sync_; }

    // Since the last successful sync; kNever before the first.
    uint32_t SinceLastSyncMs(uint32_t now_ms) const;

    // kNever when nothing is scheduled (off, no internet); 0 when due/running.
    uint32_t NextSyncInMs(uint32_t now_ms) const;

    uint16_t Successes() const { return successes_; }
    uint16_t FailuresInARow() const { return failures_; }

private:
    // now_ms is esp_timer ms and wraps every ~49 days: signed differences only.
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }
    static uint32_t Earlier(uint32_t a_ms, uint32_t b_ms) {
        return static_cast<int32_t>(a_ms - b_ms) < 0 ? a_ms : b_ms;
    }

    SyncSettings settings_ = {};
    bool usable_ = false;
    bool awaiting_ = false;
    bool have_sync_ = false;

    uint32_t next_at_ms_ = 0;
    uint32_t last_sync_ms_ = 0;
    uint32_t backoff_ms_ = 60u * 1000u;

    uint16_t successes_ = 0;
    uint16_t failures_ = 0;
};

}  // namespace timesync
