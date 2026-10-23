#pragma once

// The pure half of power-off (host-tested): which pins are latched (gpio
// hold) through deep sleep, and at what level. As FreeInk's PowerManager
// (powerDownRailsForSleep) on the X4 Pro: SD and touch power off (their
// enables are active-low: held high), the panel's RST held high so the
// controller stays in its deep sleep. Boot releases the same pins before any
// driver touches them. Wake: the Power button (RTC ext1, low).

#include <cstddef>
#include <cstdint>

#include "board_spec.h"

namespace power {

struct Hold {
    int8_t pin;
    uint8_t level;
};

inline constexpr size_t kMaxHolds = 3;

struct Holds {
    Hold items[kMaxHolds] = {};
    size_t size = 0;

    constexpr void Add(int8_t pin, uint8_t level) {
        if (pin != boards::kNoPin && size < kMaxHolds) {
            items[size++] = {pin, level};
        }
    }
};

constexpr Holds SleepHolds(const boards::Board &b) {
    Holds out;
    out.Add(b.display_pins.reset, 1);
    out.Add(b.sd_pins.power_enable, 1);
    out.Add(b.touch_pins.power_enable, 1);
    return out;
}

}  // namespace power
