#include "timesync.h"

#include <cstdio>

#include "clock_math.h"
#include "config.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_manager.h"

namespace timesync {

namespace {

constexpr const char *TAG = "timesync";

constexpr uint32_t kStackBytes = 4096;
constexpr uint32_t kPollMs = 1000;
// Our bound on one exchange: DNS + a retry on a slow link.
constexpr uint32_t kExchangeMs = 15000;

// In PSRAM: the task never touches the flash (the cache is off meanwhile).
EXT_RAM_BSS_ATTR StackType_t task_stack[kStackBytes];
StaticTask_t task_storage;
StaticSemaphore_t lock_storage;
SemaphoreHandle_t lock = nullptr;

SyncPolicy policy;
rtc::Clock *board_clock = nullptr;
bool started = false;

// Written by the task under the lock (time_t is 64-bit: no torn reads).
esp_err_t last_error = ESP_OK;
time_t last_utc = 0;
int32_t last_step_seconds = 0;

// Static, not on the task stack; only the task uses it.
char server[config::kHostSize];

struct Outcome {
    bool ok = false;
    esp_err_t error = ESP_OK;
    time_t utc = 0;
    int32_t step_seconds = 0;
};

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// syncHours 0 or no server: nothing to do (both are the file saying so).
SyncSettings SettingsFromConfig() {
    const config::Time &t = config::Get().time;
    SyncSettings settings;
    settings.enabled = t.sync_hours > 0 && t.server[0] != '\0';
    settings.interval_ms = static_cast<uint32_t>(t.sync_hours) * 3600u * 1000u;
    return settings;
}

// A client link with an address that the internet check does not call
// offline. Unknown (check off, or not answered yet) counts as usable: an SNTP
// exchange is its own reachability test.
bool InternetIsUsable() {
    if (!wifimgr::Ready()) {
        return false;
    }
    const wifimgr::Snapshot s = wifimgr::Get();
    const bool link = (s.radio.mode == wifi::Mode::kClient || s.radio.mode == wifi::Mode::kApSta) &&
                      s.radio.link == wifi::Link::kConnected && s.radio.ip != 0;
    return link && s.internet != wifimgr::Internet::kOffline;
}

// One exchange, blocking, never with the lock held.
Outcome RunExchange() {
    Outcome outcome;
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(server, sizeof server, "%s", config::Get().time.server);
    xSemaphoreGive(lock);

    // SNTP keeps the pointer: `server` lives until deinit below.
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    esp_err_t err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP did not start: %s", esp_err_to_name(err));
        outcome.error = err;
        return outcome;
    }
    // time() is what the exchange moves; esp_timer is monotonic. Both, so the
    // step excludes the seconds the exchange itself took.
    const time_t before = time(nullptr);
    const int64_t started_us = esp_timer_get_time();
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(kExchangeMs));
    // Always: a second init over a live client is refused.
    esp_netif_sntp_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no answer from %s within %u ms", server, static_cast<unsigned>(kExchangeMs));
        outcome.error = err;
        return outcome;
    }

    const time_t after = time(nullptr);
    const rtc::DateTime t = rtc::FromUnix(after);
    if (!rtc::Believable(after)) {
        ESP_LOGW(TAG, "%s answered %04u, ignored", server, t.year);
        outcome.error = ESP_ERR_INVALID_RESPONSE;
        return outcome;
    }
    outcome.ok = true;
    outcome.utc = after;
    const int64_t elapsed_s = (esp_timer_get_time() - started_us + 500000) / 1000000;
    // From an unset clock (1970) there is no step worth reporting.
    outcome.step_seconds =
        rtc::Believable(before) ? static_cast<int32_t>(after - before - elapsed_s) : 0;

    const esp_err_t stored = rtc::StoreSystemTime(*board_clock);
    if (stored != ESP_OK) {
        // The system clock is right either way; the next boot loses the head start.
        ESP_LOGW(TAG, "%s did not take the time: %s", board_clock->Name(), esp_err_to_name(stored));
    }
    ESP_LOGI(TAG, "clock set from %s: %04u-%02u-%02u %02u:%02u:%02u UTC (%+d s)", server, t.year,
             t.month, t.day, t.hour, t.minute, t.second, static_cast<int>(outcome.step_seconds));
    return outcome;
}

void Task(void *) {
    for (;;) {
        const bool usable = InternetIsUsable();
        xSemaphoreTake(lock, portMAX_DELAY);
        policy.OnInternet(usable, NowMs());
        const Action action = policy.Tick(NowMs());
        xSemaphoreGive(lock);

        if (action == Action::kSync) {
            const Outcome outcome = RunExchange();
            xSemaphoreTake(lock, portMAX_DELAY);
            last_error = outcome.error;
            if (outcome.ok) {
                last_utc = outcome.utc;
                last_step_seconds = outcome.step_seconds;
            }
            policy.OnResult(outcome.ok, NowMs());
            xSemaphoreGive(lock);
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

esp_err_t Init(rtc::Clock *clock) {
    if (started) {
        return ESP_OK;
    }
    if (clock == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    board_clock = clock;
    lock = xSemaphoreCreateMutexStatic(&lock_storage);
    policy.Configure(SettingsFromConfig(), NowMs());
    if (xTaskCreateStatic(&Task, "timesync", kStackBytes, nullptr, 3, task_stack,
                          &task_storage) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    started = true;

    const config::Time &t = config::Get().time;
    if (t.sync_hours == 0 || t.server[0] == '\0') {
        ESP_LOGI(TAG, "time sync off");
    } else {
        ESP_LOGI(TAG, "time sync every %u h from %s, and when the internet comes back",
                 static_cast<unsigned>(t.sync_hours), t.server);
    }
    return ESP_OK;
}

bool Ready() { return started; }

void Apply() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    policy.Configure(SettingsFromConfig(), NowMs());
    xSemaphoreGive(lock);
}

void SyncNow() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    policy.SyncNow(NowMs());
    xSemaphoreGive(lock);
}

Status Get() {
    Status status;
    if (!started) {
        return status;
    }
    const uint32_t now = NowMs();
    xSemaphoreTake(lock, portMAX_DELAY);
    status.enabled = policy.Enabled();
    status.internet = policy.InternetIsUsable();
    status.syncing = policy.Syncing();
    status.ever_synced = policy.EverSynced();
    status.since_last_ms = policy.SinceLastSyncMs(now);
    status.next_in_ms = policy.NextSyncInMs(now);
    status.successes = policy.Successes();
    status.failures = policy.FailuresInARow();
    status.last_utc = last_utc;
    status.last_step_seconds = last_step_seconds;
    status.last_error = last_error;
    xSemaphoreGive(lock);
    return status;
}

}  // namespace timesync
