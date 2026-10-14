#include "clock.h"

#include <sys/time.h>

#include "clock_math.h"
#include "esp_log.h"

namespace rtc {

namespace {
constexpr const char *TAG = "clock";
}  // namespace

int64_t SystemTimeUs() {
    timeval tv = {};
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

esp_err_t SetSystemTimeFrom(Clock &clock) {
    int64_t utc_us = 0;
    bool valid = false;
    const esp_err_t err = clock.Read(&utc_us, &valid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: read failed: %s", clock.Name(), esp_err_to_name(err));
        return err;
    }
    if (!valid) {
        ESP_LOGI(TAG, "%s: no time yet, system clock left unset", clock.Name());
        return ESP_ERR_INVALID_STATE;
    }
    const timeval tv = {.tv_sec = static_cast<time_t>(utc_us / 1000000),
                        .tv_usec = static_cast<suseconds_t>(utc_us % 1000000)};
    settimeofday(&tv, nullptr);
    const DateTime t = FromUnix(utc_us / 1000000);
    ESP_LOGI(TAG, "system clock from %s: %04u-%02u-%02u %02u:%02u:%02u UTC", clock.Name(), t.year,
             t.month, t.day, t.hour, t.minute, t.second);
    return ESP_OK;
}

esp_err_t StoreSystemTime(Clock &clock) {
    const int64_t now = SystemTimeUs();
    if (!Believable(now / 1000000)) {
        return ESP_ERR_INVALID_STATE;
    }
    return clock.Write(now);
}

}  // namespace rtc
