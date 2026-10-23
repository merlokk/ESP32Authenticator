#pragma once

// The board's frontlight (X4 Pro: cool on GPIO8, warm on GPIO9, active high),
// on two LEDC channels (frontlight_logic.h for the curve and the mix). Off on
// a board without one: every call is a no-op.

#include <cstdint>

#include "esp_err.h"

namespace frontlight {

// After boards::Init. Starts dark.
esp_err_t Init();
bool Present();

// 0..100 % each; brightness 0 is off. Fades over kFadeMs.
esp_err_t Set(uint8_t brightness, uint8_t warmth);

uint8_t Brightness();
uint8_t Warmth();

// Console `light`.
void PrintInfo();

}  // namespace frontlight
