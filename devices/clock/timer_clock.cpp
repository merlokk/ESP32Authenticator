#include "timer_clock.h"

#include "clock_math.h"
#include "esp_attr.h"
#include "esp_rtc_time.h"
#include "freertos/FreeRTOS.h"

namespace rtc {

namespace {

// Kept by a software reset, garbage after power-on (TimerRead rejects it).
RTC_NOINIT_ATTR TimerState state;

// 64-bit fields on a 32-bit core: no torn reads between tasks.
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

esp_err_t TimerClock::Read(int64_t *utc_us, bool *valid) {
    if (utc_us == nullptr || valid == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&lock);
    const TimerState copy = state;
    portEXIT_CRITICAL(&lock);
    int64_t value = 0;
    *valid = TimerRead(copy, esp_rtc_get_time_us(), &value) && Believable(value / 1000000);
    *utc_us = *valid ? value : 0;
    return ESP_OK;
}

esp_err_t TimerClock::Write(int64_t utc_us) {
    if (!Believable(utc_us / 1000000)) {
        return ESP_ERR_INVALID_ARG;
    }
    const TimerState fresh = TimerSet(utc_us, esp_rtc_get_time_us());
    portENTER_CRITICAL(&lock);
    state = fresh;
    portEXIT_CRITICAL(&lock);
    return ESP_OK;
}

}  // namespace rtc
