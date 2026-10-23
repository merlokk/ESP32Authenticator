#pragma once

// Power-off: the X4 Pro has no power switch; "off" is ESP32 deep sleep with
// the panel showing a wallpaper (ui::ShowWallpaper), woken by the Power
// button, which boots afresh. Nothing decrypted survives, but for a secret
// handed to Keep (the vault key, config.json power.keepPinHours): RTC memory
// through that one sleep, good for that many hours from it.
//
// Off on its own after power.idleMinutes with nothing done (NoteActivity);
// a USB host (the console) or the BLE pairing window keeps it on.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace power {

// First thing in app_main: releases the pins latched for the last sleep
// (power_logic.h), logs what woke the chip, and moves a kept secret out of
// RTC memory (TakeKept hands it over).
void Boot();

// The idle watch (config.json power.idleMinutes, read live). After config::Init.
esp_err_t Start();

// Something the user did (keys, touch, typing, FIDO2, a web request): the
// idle count starts again. Any task, cheap.
void NoteActivity();

// Long press of Power, console `off`, the idle watch: runs Off in its own
// task (the caller may be the keypad task). `lock`: nothing kept, the PIN
// after the wake. `wake_seconds` (tests): a timer wakes it too, 0: only
// Power. ESP_ERR_INVALID_STATE: already on its way; ESP_ERR_NOT_SUPPORTED:
// no Power button to wake it (dev board).
esp_err_t RequestOff(bool lock = false, uint32_t wake_seconds = 0);

// Called by Off right before the sleep, from its task: `lock` false is the
// time to hand a secret to Keep.
void SetSleepHook(void (*hook)(bool lock));

// `size` bytes (up to kKeptBytes) kept through the coming sleep for `hours`
// from now (the system clock, believable). False: not kept.
bool Keep(const void *data, size_t size, uint32_t hours);

// After a deep-sleep wake (Boot, then the board's clock in the system clock):
// the kept secret, if still in time. Once: wiped either way.
bool TakeKept(void *data, size_t size);

// Frontlight and BLE off, the wallpaper (the screen as it was if there is
// none), the panel asleep, waits for Power to be released, the sleep hook,
// deep sleep. A sleep the chip refuses: a restart.
[[noreturn]] void Off(bool lock, uint32_t wake_seconds);

}  // namespace power
