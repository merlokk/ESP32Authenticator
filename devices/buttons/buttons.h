#pragma once

#include "button_logic.h"
#include "driver/gpio.h"
#include "esp_err.h"

// Push buttons on GPIOs, read by polling and debounced (button_logic.h). Pins
// come from the board (boards::Button(), devices/boards). No task of its own:
// the owner polls; one owner per button (the state is not locked).

namespace buttons {

class Button {
public:
    constexpr Button(gpio_num_t gpio, bool active_low = true)
        : gpio_(gpio), active_low_(active_low) {}

    // GPIO_NUM_NC: the board has no such button (never pressed).
    bool Present() const { return gpio_ != GPIO_NUM_NC; }

    // Input with a pull towards the released level; adopts the current level
    // without an edge.
    esp_err_t Init();

    // The pin right now, not debounced.
    bool RawPressed() const;

    // Samples the pin; the debounced edge, if any.
    Event Poll();

    // Debounced state as of the last Poll().
    bool Pressed() const { return state_.Pressed(); }

    // How long the debounced state has held, as of now.
    uint32_t StableMs() const;

    // Blocks until held for `hold_ms` (true) or released (false, at once when
    // not down).
    bool HeldFor(uint32_t hold_ms);

    gpio_num_t Gpio() const { return gpio_; }

private:
    gpio_num_t gpio_;
    bool active_low_;
    Debounce state_;
};

}  // namespace buttons
