#pragma once

// The selected board's devices (board.h says which exist and where). Only
// carries out what the board class says.

#include "board.h"
#include "buttons.h"
#include "clock.h"
#include "esp_err.h"

namespace boards {

// The LED first (red: the firmware is booting), then the clock (a hardware RTC
// when the board has one and a driver exists, else the timer clock), which
// seeds the system clock. First thing in app_main.
esp_err_t Init();

// app_main is done: the LED goes to "ready" (green, breathing, idle ceiling).
void BootDone();

// Keys going to the host: the LED glows blue until 1 s after the last call.
void LedTyping();

// FIDO2 waits for the button: the LED blinks yellow until called with false.
void LedWaitingForUser(bool waiting);

// LED brightness ceilings in percent (config.json `led`); no-op without an LED.
void SetLedBrightness(uint8_t percent, uint8_t idle_percent);

// The clock that carries UTC across reboots. Valid before Init() too (timer).
rtc::Clock &Clock();

// The board's button; Present() false when the board has none.
buttons::Button Button(ButtonId id);

// Name, features and pins (console `board`).
void PrintInfo();

// `[clock]` section of `hwinfo`: the board's clock, its value, the system clock.
void PrintClock();

}  // namespace boards
