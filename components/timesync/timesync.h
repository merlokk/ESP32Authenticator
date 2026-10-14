#pragma once

// SNTP: sets the system clock from `time.server` (config.json) and writes it
// back to the board's clock, so the next boot starts from it. When: see
// sync_policy.h. Internet: wifimgr's answer. UTC only, the zone is never read.
// An answer outside 2024..2099 is refused and counted as a failure.

#include <cstdint>
#include <ctime>

#include "clock.h"
#include "esp_err.h"
#include "sync_policy.h"

namespace timesync {

struct Status {
    bool enabled = false;
    bool internet = false;  // a client link with an address, not known offline
    bool syncing = false;
    bool ever_synced = false;
    uint32_t since_last_ms = kNever;
    uint32_t next_in_ms = kNever;
    uint16_t successes = 0;
    uint16_t failures = 0;  // in a row
    time_t last_utc = 0;     // when the last good sync happened
    // How far it moved the clock (exchange time excluded): a clock that keeps
    // being stepped by seconds is a clock to distrust.
    int32_t last_step_seconds = 0;
    esp_err_t last_error = ESP_OK;
};

// Starts the sync task. The clock is borrowed (boards::Clock()).
esp_err_t Init(rtc::Clock *clock);
bool Ready();

// Re-reads time.server / time.syncHours; keeps the last sync.
void Apply();

// Sync at the next tick (still needs an internet and a server).
void SyncNow();

Status Get();

}  // namespace timesync
