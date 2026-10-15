#include "buttons.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace buttons {

namespace {

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

}  // namespace

esp_err_t Button::Init() {
    if (!Present()) {
        return ESP_ERR_NOT_FOUND;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio_,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_low_ ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_low_ ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        state_.Reset(RawPressed(), NowMs());
    }
    return err;
}

bool Button::RawPressed() const {
    return Present() && (gpio_get_level(gpio_) == 0) == active_low_;
}

Event Button::Poll() { return Present() ? state_.Update(RawPressed(), NowMs()) : Event::kNone; }

uint32_t Button::StableMs() const { return state_.StableMs(NowMs()); }

bool Button::HeldFor(uint32_t hold_ms) {
    const uint32_t started = NowMs();
    for (;;) {
        Poll();
        if (!Pressed()) {
            return false;
        }
        if (NowMs() - started >= hold_ms) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

}  // namespace buttons
