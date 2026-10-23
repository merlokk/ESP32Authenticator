#include "bm8563_clock.h"

#include <cstring>

#include "bm8563_logic.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace rtc {

namespace {

namespace bm = bm8563;

constexpr const char *TAG = "bm8563";

// Read: wait at most this long for the next tick.
constexpr int64_t kTickWaitUs = 1100000;
// Write: time for the writes before the chip is started.
constexpr int64_t kWriteLeadUs = 50000;
// Write: sleep to this close to the start, then spin.
constexpr int64_t kSpinUs = 3000;

}  // namespace

esp_err_t Bm8563Clock::Init() {
    present_ = false;
    auto lease = bus_.Acquire();
    if (!lease) {
        return bus_.Ready() ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    if (lease.Probe(bm::kAddress) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t control1 = 0;
    const esp_err_t err = lease.ReadRegister(bm::kAddress, bm::kRegControl1, &control1, 1);
    if (err != ESP_OK) {
        return err;
    }
    // Control_2 (alarm/timer interrupts) and CLKOUT are left as found.
    present_ = true;
    ESP_LOGI(TAG, "found at 0x%02x, control_1 0x%02x%s", bm::kAddress, control1,
             (control1 & bm::kControl1Stop) != 0 ? " (stopped)" : "");
    return ESP_OK;
}

esp_err_t Bm8563Clock::ReadCounters(int64_t *seconds, bool *valid) {
    uint8_t control1 = 0;
    uint8_t raw[bm::kCounterCount] = {};
    {
        auto lease = bus_.Acquire();
        if (!lease) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err = lease.ReadRegister(bm::kAddress, bm::kRegControl1, &control1, 1);
        if (err == ESP_OK) {
            err = lease.ReadRegister(bm::kAddress, bm::kRegSeconds, raw, sizeof raw);
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    DateTime t = {};
    const bm::Decoded d = bm::Decode(raw, &t);
    if ((control1 & bm::kControl1Stop) != 0) {
        state_ = "stopped (STOP set)";
        *valid = false;
    } else {
        state_ = bm::Name(d);
        *valid = d == bm::Decoded::kOk;
    }
    *seconds = *valid ? ToUnix(t) : 0;
    return ESP_OK;
}

esp_err_t Bm8563Clock::Read(int64_t *utc_us, bool *valid) {
    if (utc_us == nullptr || valid == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *utc_us = 0;
    *valid = false;
    if (!present_) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t first = 0;
    esp_err_t err = ReadCounters(&first, valid);
    if (err != ESP_OK || !*valid) {
        return err;
    }
    // The counters only say which second it is: wait for the next one to
    // begin, then the time is that second plus what has passed since. The
    // seconds register alone, back to back (~0.1 ms a read; the tick is
    // 10 ms, too coarse), each under its own lease so others get the bus.
    uint8_t last = 0xFF;
    const int64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < kTickWaitUs) {
        uint8_t seconds = 0;
        {
            auto lease = bus_.Acquire();
            err = lease ? lease.ReadRegister(bm::kAddress, bm::kRegSeconds, &seconds, 1)
                        : ESP_ERR_TIMEOUT;
        }
        const int64_t seen_at = esp_timer_get_time();
        if (err != ESP_OK) {
            *valid = false;
            return err;
        }
        if (last != 0xFF && seconds != last) {
            int64_t now = 0;
            err = ReadCounters(&now, valid);
            if (err != ESP_OK || !*valid || now != first + 1) {
                *valid = false;  // a write in between, or a jump: no time
                return err;
            }
            *utc_us = now * 1000000 + (esp_timer_get_time() - seen_at);
            return ESP_OK;
        }
        last = seconds;
        taskYIELD();
    }
    // No tick in 1.1 s: the oscillator is not running.
    state_ = "not ticking";
    *valid = false;
    return ESP_OK;
}

esp_err_t Bm8563Clock::SetStop(bool stop) {
    auto lease = bus_.Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t control1 = 0;
    const esp_err_t err = lease.ReadRegister(bm::kAddress, bm::kRegControl1, &control1, 1);
    if (err != ESP_OK) {
        return err;
    }
    control1 = stop ? static_cast<uint8_t>(control1 | bm::kControl1Stop)
                    : static_cast<uint8_t>(control1 & ~bm::kControl1Stop);
    return lease.WriteRegister(bm::kAddress, bm::kRegControl1, control1);
}

esp_err_t Bm8563Clock::Write(int64_t utc_us) {
    if (!present_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!Believable(utc_us / 1000000)) {
        return ESP_ERR_INVALID_ARG;
    }
    // utc_us is "now" on the caller's clock: carry it on the CPU timer.
    const int64_t offset = utc_us - esp_timer_get_time();
    const bm::WritePlan plan = bm::PlanWrite(utc_us, kWriteLeadUs);

    uint8_t counters[bm::kCounterCount] = {};
    bm::Encode(plan.seconds, counters);  // VL written 0: the time is good again
    uint8_t payload[1 + bm::kCounterCount] = {bm::kRegSeconds};
    memcpy(payload + 1, counters, sizeof counters);

    esp_err_t err = SetStop(true);
    if (err == ESP_OK) {
        auto lease = bus_.Acquire();
        err = lease ? lease.Write(bm::kAddress, payload, sizeof payload) : ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        const int64_t start_at = plan.start_at_us - offset;  // on the CPU timer
        const int64_t sleep_us = start_at - kSpinUs - esp_timer_get_time();
        if (sleep_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
        }
        while (esp_timer_get_time() < start_at) {
        }
    }
    // Start it whatever happened above: one bad write must not leave a dead
    // clock. Once more if the bus was busy.
    esp_err_t start = SetStop(false);
    if (start != ESP_OK) {
        start = SetStop(false);
    }
    if (err == ESP_OK && start != ESP_OK) {
        err = start;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write: %s", esp_err_to_name(err));
    }
    return err;
}

}  // namespace rtc
