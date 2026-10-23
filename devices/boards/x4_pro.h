#pragma once

// XTEINK X4 Pro (hardware.md). Pins from TuyaOpen board_config.h and the
// FreeInk X4 Pro doc. Power-up: peripheral rail high, touch power low, SD power
// pulsed high -> low.

#include "board_spec.h"

namespace boards {

class X4Pro final : public Board {
public:
    constexpr X4Pro() {
        name = "XTEINK X4 Pro";
        ble_name = "ESP32 Auth X4Pro";
        hw_clock = HwClockType::kBm8563;
        display = DisplayType::kEink800x480;
        touch = TouchType::kGt911;
        battery = BatteryType::kCw2017;
        has_sd_card = true;
        has_frontlight = true;
        fido_max_credentials = 20;  // SD card; more needs a higher kMaxFidoCredentials
        fido_reset_window_s = 10;   // 0 once the e-ink UI confirms a reset
        max_records = 500;          // SD card

        buttons[static_cast<size_t>(ButtonId::kLeft)] = {0, true};  // also the ROM strap
        buttons[static_cast<size_t>(ButtonId::kRight)] = {7, true};
        buttons[static_cast<size_t>(ButtonId::kPower)] = {3, true};

        i2c = {.scl = 38, .sda = 39};
        display_pins = {.mosi = 11, .sclk = 12, .cs = 13, .dc = 18, .reset = 14, .busy = 6};
        touch_pins = {.reset = 4, .interrupt = 10, .power_enable = 2};
        sd_pins = {.clk = 41, .cmd = 42, .d0 = 40, .power_enable = 5};
        frontlight_pins = {.cool = 8, .warm = 9};
        power_pins = {.peripheral_rail = 1, .charger_status = 21};
    }
};

}  // namespace boards
