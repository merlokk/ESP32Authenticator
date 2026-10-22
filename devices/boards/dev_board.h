#pragma once

// Bare ESP32-S3 dev board (YD-ESP32-S3 type, as in approver-esp32-yubikey):
// N16R8 module, BOOT button, one WS2812 LED, nothing else. Console on the
// native USB Serial/JTAG port.

#include "board_spec.h"

namespace boards {

class DevBoard final : public Board {
public:
    constexpr DevBoard() {
        name = "ESP32-S3 dev board";
        led = LedType::kWs2812;
        fido_max_credentials = 20;  // fido.bin <= 22 KB (+ .tmp on save) in 68 KB SPIFFS
        fido_reset_window_s = 10;  // no display
        max_records = 50;  // SPIFFS
        buttons[static_cast<size_t>(ButtonId::kLeft)] = {0, true};  // BOOT, also the ROM strap
        led_pins.data = 48;
    }
};

}  // namespace boards
