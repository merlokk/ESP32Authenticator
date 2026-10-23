#pragma once

// Clock on the BM8563 RTC (bm8563_logic.h) on the board's leased I2C bus:
// battery-backed, keeps UTC with the power off. One-second counters; Read
// waits for the next tick (up to 1 s) to give the time to the millisecond,
// Write starts the chip so its second ticks with the caller's.

#include "clock.h"
#include "i2c_bus.h"

namespace rtc {

class Bm8563Clock final : public Clock {
public:
    explicit Bm8563Clock(i2cbus::Bus &bus) : bus_(bus) {}

    const char *Name() const override { return "BM8563"; }
    bool KeepsTimeWhenOff() const override { return true; }
    // ESP_ERR_NOT_FOUND: nothing answers at 0x51.
    esp_err_t Init() override;
    esp_err_t Read(int64_t *utc_us, bool *valid) override;
    esp_err_t Write(int64_t utc_us) override;

    // What the last read found (bm8563::Decoded, or STOP set); for `date`.
    const char *State() const { return state_; }

private:
    esp_err_t ReadCounters(int64_t *seconds, bool *valid);
    esp_err_t SetStop(bool stop);

    i2cbus::Bus &bus_;
    bool present_ = false;
    const char *state_ = "not read";
};

}  // namespace rtc
