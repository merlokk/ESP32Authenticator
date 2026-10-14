#pragma once

// LVGL v9: 1-bit colour, all memory in PSRAM (lv_mem_psram.c), settings in
// sdkconfig.defaults. No display or input device yet: the X4 Pro e-ink panel and
// GT911 touch drivers attach here later, with the task that runs
// lv_timer_handler.

#include "esp_err.h"

namespace ui {

// lv_init() and the tick source (esp_timer). Safe to call more than once.
esp_err_t Init();
bool Ready();

// "9.4.0".
const char *LvglVersion();

}  // namespace ui
