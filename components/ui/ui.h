#pragma once

// LVGL v9: 1-bit colour, all memory in PSRAM (lv_mem_psram.c), settings in
// sdkconfig.defaults. The e-ink panel is a 480x800 portrait display
// (ui_logic.h turns it onto the landscape panel), redrawn by the "ui" task
// that runs lv_timer_handler. No input device yet (GT911 touch comes later).

#include "esp_err.h"

namespace ui {

// lv_init() and the tick source (esp_timer); with a panel (epd::Init first)
// the display, the boot screen and the ui task. Safe to call more than once.
esp_err_t Init();
bool Ready();
bool HasDisplay();

// LVGL is not thread-safe (LV_OS_NONE): other tasks touch widgets only
// between Lock and Unlock. Recursive.
void Lock();
void Unlock();

// "9.4.0".
const char *LvglVersion();

// Console `version`/`ui`: display, refresh count, the last flush, task stack.
void PrintInfo();

}  // namespace ui
