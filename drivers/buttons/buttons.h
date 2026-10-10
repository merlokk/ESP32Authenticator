#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

// Push buttons on GPIOs, read by polling.

namespace buttons {

class Button {
public:
    constexpr Button(gpio_num_t gpio, bool active_low = true)
        : gpio_(gpio), active_low_(active_low) {}

    // Input with a pull towards the released level.
    esp_err_t Init() const;

    bool Pressed() const;

    gpio_num_t Gpio() const { return gpio_; }

private:
    gpio_num_t gpio_;
    bool active_low_;
};

// X4 Pro buttons (hardware.md). The dev board has only BOOT, on GPIO0 = Left.
inline constexpr Button kLeft{GPIO_NUM_0};
inline constexpr Button kRight{GPIO_NUM_7};
inline constexpr Button kPower{GPIO_NUM_3};

}  // namespace buttons
