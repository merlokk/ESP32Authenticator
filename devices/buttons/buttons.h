#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

// Push buttons on GPIOs, read by polling. Pins come from the board
// (boards::Button(), devices/boards).

namespace buttons {

class Button {
public:
    constexpr Button(gpio_num_t gpio, bool active_low = true)
        : gpio_(gpio), active_low_(active_low) {}

    // GPIO_NUM_NC: the board has no such button (never pressed).
    bool Present() const { return gpio_ != GPIO_NUM_NC; }

    // Input with a pull towards the released level.
    esp_err_t Init() const;

    bool Pressed() const;

    gpio_num_t Gpio() const { return gpio_; }

private:
    gpio_num_t gpio_;
    bool active_low_;
};

}  // namespace buttons
