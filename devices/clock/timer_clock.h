#pragma once

// Clock on the chip's RTC timer (esp_rtc_get_time_us): no hardware needed.
// Keeps the time across software resets (reboot, OTA, panic), loses it at
// power-on. Used until a board has a battery-backed RTC.

#include "clock.h"

namespace rtc {

class TimerClock final : public Clock {
public:
    const char *Name() const override { return "cpu timer"; }
    bool KeepsTimeWhenOff() const override { return false; }
    esp_err_t Init() override { return ESP_OK; }
    esp_err_t Read(int64_t *utc_us, bool *valid) override;
    esp_err_t Write(int64_t utc_us) override;
};

}  // namespace rtc
