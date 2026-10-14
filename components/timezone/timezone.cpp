#include "timezone.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"

namespace tz {

namespace {

constexpr const char *TAG = "tz";

char current[kMaxPosixLength] = "UTC0";

}  // namespace

esp_err_t ApplyZone(const char *zone) {
    const char *posix = Resolve(zone);
    if (posix == nullptr || strlen(posix) >= sizeof current) {
        return ESP_ERR_INVALID_ARG;
    }
    if (setenv("TZ", posix, 1) != 0) {
        return ESP_FAIL;
    }
    tzset();
    snprintf(current, sizeof current, "%s", posix);
    ESP_LOGI(TAG, "%s (TZ=%s)", zone, posix);
    return ESP_OK;
}

const char *Current() { return current; }

int OffsetSeconds(time_t when) {
    if (when == 0) {
        when = time(nullptr);
    }
    // The local breakdown read back as if it were UTC: the difference is the
    // offset, DST included (timegm, not mktime: no isdst guessing, right sign).
    struct tm local = {};
    localtime_r(&when, &local);
    return static_cast<int>(timegm(&local) - when);
}

bool IsDaylightSaving(time_t when) {
    if (when == 0) {
        when = time(nullptr);
    }
    struct tm local = {};
    localtime_r(&when, &local);
    return local.tm_isdst > 0;
}

}  // namespace tz
