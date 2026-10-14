#pragma once

// A clock that carries UTC to the next boot. The system clock (`time()`) is
// what everything reads; a Clock seeds it at boot and is written back when the
// time is set (SNTP, `date set`).
//
// Implementations: TimerClock (the chip's RTC timer, survives software resets
// only). A battery-backed chip (the X4 Pro's BM8563) is a second one later.

#include <cstdint>

#include "esp_err.h"

namespace rtc {

class Clock {
public:
    virtual ~Clock() = default;

    virtual const char *Name() const = 0;

    // Keeps counting with the power off (battery-backed chip).
    virtual bool KeepsTimeWhenOff() const = 0;

    virtual esp_err_t Init() = 0;

    // ESP_OK with *valid false: read fine, but no time to trust (never set,
    // power lost, out of the believable range).
    virtual esp_err_t Read(int64_t *utc_us, bool *valid) = 0;

    virtual esp_err_t Write(int64_t utc_us) = 0;
};

// Sets the system clock from `clock` when it has a believable time; otherwise
// leaves it alone (unset reads as 1970, which nothing mistakes for now).
esp_err_t SetSystemTimeFrom(Clock &clock);

// Writes the system clock into `clock`, when the system clock is believable.
esp_err_t StoreSystemTime(Clock &clock);

// The system clock in microseconds since the epoch, UTC.
int64_t SystemTimeUs();

}  // namespace rtc
