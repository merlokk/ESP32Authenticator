#include "ui.h"

#include <cinttypes>
#include <cstdio>

#include "epd.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui_logic.h"

namespace ui {

namespace {

constexpr const char *TAG = "ui";

constexpr uint32_t kStackBytes = 8192;  // LVGL's software renderer
constexpr int kPriority = 3;            // below the keypad
constexpr uint32_t kMinSleepMs = 10;
constexpr uint32_t kMaxSleepMs = 200;   // LV_DEF_REFR_PERIOD

// An I1 draw buffer starts with its 2-colour palette.
constexpr size_t kPaletteBytes = LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * 4;
constexpr size_t kBufBytes = kPaletteBytes + kScreenBytes;

bool started = false;
char version[16] = {};

SemaphoreHandle_t mutex = nullptr;
StaticSemaphore_t mutex_storage;
StackType_t task_stack[kStackBytes];
StaticTask_t task_storage;
TaskHandle_t task = nullptr;

lv_display_t *display = nullptr;
uint8_t *screen = nullptr;  // LVGL's portrait buffer (PSRAM)
uint8_t *frame = nullptr;   // the same, turned for the panel (PSRAM)
RefreshPlan plan;
uint32_t refreshes = 0;
uint32_t last_rotate_us = 0;
esp_err_t last_err = ESP_OK;

uint32_t TickMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// DIRECT mode: LVGL redraws the dirty areas in place; the panel takes whole
// frames, so only the last area of a refresh goes out.
void Flush(lv_display_t *disp, const lv_area_t *, uint8_t *px_map) {
    if (lv_display_flush_is_last(disp)) {
        const int64_t start = esp_timer_get_time();
        ToPanel(px_map + kPaletteBytes, frame);
        last_rotate_us = static_cast<uint32_t>(esp_timer_get_time() - start);
        last_err = epd::Refresh(frame, plan.NextIsFull() ? epd::Mode::kFull : epd::Mode::kFast);
        ++refreshes;
        if (last_err != ESP_OK) {
            ESP_LOGE(TAG, "refresh: %s", esp_err_to_name(last_err));
        }
    }
    lv_display_flush_ready(disp);
}

// Until the real screens: the name, the firmware and a 2 px frame inside the
// bezel (all of it shows when the insets are right).
void BootScreen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *border = lv_obj_create(scr);
    lv_obj_remove_style_all(border);
    lv_obj_set_pos(border, kInsetLeft, kInsetTop);
    lv_obj_set_size(border, kWidth - kInsetLeft - kInsetRight, kHeight - kInsetTop - kInsetBottom);
    lv_obj_set_style_border_width(border, 2, 0);
    lv_obj_set_style_border_color(border, lv_color_black(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32 Authenticator");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *fw = lv_label_create(scr);
    lv_label_set_text_fmt(fw, "firmware %s", esp_app_get_description()->version);
    lv_obj_set_style_text_font(fw, &lv_font_montserrat_20, 0);
    lv_obj_align_to(fw, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);

    lv_obj_t *lv = lv_label_create(scr);
    lv_label_set_text_fmt(lv, "LVGL %s, %ux%u", LvglVersion(), kWidth, kHeight);
    lv_obj_set_style_text_font(lv, &lv_font_montserrat_20, 0);
    lv_obj_align(lv, LV_ALIGN_BOTTOM_MID, 0, -40);
}

void Task(void *) {
    for (;;) {
        Lock();
        uint32_t ms = lv_timer_handler();
        Unlock();
        ms = ms < kMinSleepMs ? kMinSleepMs : ms > kMaxSleepMs ? kMaxSleepMs : ms;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

esp_err_t StartDisplay() {
    screen = static_cast<uint8_t *>(heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, kBufBytes,
                                                             MALLOC_CAP_SPIRAM));
    frame = static_cast<uint8_t *>(heap_caps_malloc(epd::kFrameBytes, MALLOC_CAP_SPIRAM));
    if (screen == nullptr || frame == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    display = lv_display_create(kWidth, kHeight);
    if (display == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(display, screen, nullptr, kBufBytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, Flush);
    BootScreen();
    // ESP-IDF counts the stack depth in bytes.
    task = xTaskCreateStatic(&Task, "ui", kStackBytes, nullptr, kPriority, task_stack,
                             &task_storage);
    return task != nullptr ? ESP_OK : ESP_FAIL;
}

}  // namespace

esp_err_t Init() {
    if (started) {
        return ESP_OK;
    }
    mutex = xSemaphoreCreateRecursiveMutexStatic(&mutex_storage);
    lv_init();
    lv_tick_set_cb(&TickMs);
    started = true;
    if (!epd::Ready()) {
        ESP_LOGI(TAG, "LVGL %s, %d-bit colour, no display", LvglVersion(), LV_COLOR_DEPTH);
        return ESP_OK;
    }
    const esp_err_t err = StartDisplay();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "LVGL %s, %d-bit colour, %ux%u portrait", LvglVersion(), LV_COLOR_DEPTH, kWidth,
             kHeight);
    return ESP_OK;
}

bool Ready() { return started; }

bool HasDisplay() { return task != nullptr; }

void Lock() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }

void Unlock() { xSemaphoreGiveRecursive(mutex); }

const char *LvglVersion() {
    if (version[0] == '\0') {
        snprintf(version, sizeof version, "%d.%d.%d", lv_version_major(), lv_version_minor(),
                 lv_version_patch());
    }
    return version;
}

void PrintInfo() {
    if (!HasDisplay()) {
        printf("ui: no display\n");
        return;
    }
    printf("ui: %ux%u portrait, %" PRIu32 " refreshes (full every %u), last %s, turn %" PRIu32
           " us, stack free %u B\n",
           kWidth, kHeight, refreshes, RefreshPlan::kFullEvery, esp_err_to_name(last_err),
           last_rotate_us, static_cast<unsigned>(uxTaskGetStackHighWaterMark(task)));
}

}  // namespace ui
