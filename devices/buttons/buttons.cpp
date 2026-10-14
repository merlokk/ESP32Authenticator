#include "buttons.h"

namespace buttons {

esp_err_t Button::Init() const {
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
    return gpio_config(&io);
}

bool Button::Pressed() const {
    return Present() && (gpio_get_level(gpio_) == 0) == active_low_;
}

}  // namespace buttons
