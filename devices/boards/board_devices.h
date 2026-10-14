#pragma once

// The selected board's devices (board.h says which exist and where). Only
// carries out what the board class says.

#include "board.h"
#include "buttons.h"
#include "clock.h"
#include "esp_err.h"

namespace boards {

// Picks the clock (a hardware RTC when the board has one and a driver exists,
// else the timer clock), seeds the system clock from it. Before config and Wi-Fi.
esp_err_t Init();

// The clock that carries UTC across reboots. Valid before Init() too (timer).
rtc::Clock &Clock();

// The board's button; Present() false when the board has none.
buttons::Button Button(ButtonId id);

// Name, features and pins (console `board`).
void PrintInfo();

// `[clock]` section of `hwinfo`: the board's clock, its value, the system clock.
void PrintClock();

}  // namespace boards
