#pragma once

// Power-off: the X4 Pro has no power switch; "off" is ESP32 deep sleep with
// the panel showing a wallpaper (ui::ShowWallpaper), woken by the Power
// button, which boots afresh (the PIN again: nothing decrypted survives).

#include "esp_err.h"

namespace power {

// First thing in app_main: releases the pins latched for the last sleep
// (power_logic.h) and logs what woke the chip.
void Boot();

// Long press of Power, console `off`: runs Off in its own task (the caller
// may be the keypad task). ESP_ERR_INVALID_STATE: already on its way;
// ESP_ERR_NOT_SUPPORTED: no Power button to wake it (dev board).
esp_err_t RequestOff();

// Frontlight off, the wallpaper (the screen as it was if there is none), the
// panel asleep, waits for Power to be released, deep sleep.
[[noreturn]] void Off();

}  // namespace power
