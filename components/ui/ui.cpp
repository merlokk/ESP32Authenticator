#include "ui.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

namespace ui {

namespace {

constexpr const char *TAG = "ui";

bool started = false;
char version[16] = {};

uint32_t TickMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

}  // namespace

esp_err_t Init() {
    if (started) {
        return ESP_OK;
    }
    lv_init();
    lv_tick_set_cb(&TickMs);
    started = true;
    ESP_LOGI(TAG, "LVGL %s, %d-bit colour, no display yet", LvglVersion(), LV_COLOR_DEPTH);
    return ESP_OK;
}

bool Ready() { return started; }

const char *LvglVersion() {
    if (version[0] == '\0') {
        snprintf(version, sizeof version, "%d.%d.%d", lv_version_major(), lv_version_minor(),
                 lv_version_patch());
    }
    return version;
}

}  // namespace ui
