#pragma once

// The board's frontlight (X4 Pro: cool on GPIO8, warm on GPIO9, active high),
// on two LEDC channels (frontlight_logic.h for the curve and the mix). A level
// (brightness, warmth) and an on/off state: the level is what On() lights,
// config.json keeps it; the state is not kept, the light starts off. On a
// board without one every call is a no-op.

#include <cstdint>

#include "esp_err.h"

namespace frontlight {

// After boards::Init. Starts off.
esp_err_t Init();
bool Present();

// The level to light at, 1..100 % brightness (0 counts as 1), 0..100 %
// warmth. Applied at once when on. Fades over kFadeMs.
esp_err_t SetLevel(uint8_t brightness, uint8_t warmth);
esp_err_t On();
esp_err_t Off();
esp_err_t Toggle();
bool IsOn();

uint8_t Brightness();
uint8_t Warmth();

// Console `light`.
void PrintInfo();

}  // namespace frontlight
