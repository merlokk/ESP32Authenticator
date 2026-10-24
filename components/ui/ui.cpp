#include "ui.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "epd.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "touch.h"
#include "ui_logic.h"

namespace ui {

void StartPinPad();  // pin_pad.cpp

namespace {

constexpr const char *TAG = "ui";

constexpr uint32_t kStackBytes = 8192;  // LVGL's software renderer
constexpr int kPriority = 3;            // below the keypad
constexpr uint32_t kMinSleepMs = 10;
constexpr uint32_t kMaxSleepMs = 200;   // LV_DEF_REFR_PERIOD
// LVGL reads an input device every LV_DEF_REFR_PERIOD (200 ms) by default: a
// short tap would slip between two reads.
constexpr uint32_t kTouchReadMs = 20;

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
uint8_t *screen = nullptr;   // LVGL's portrait buffer (PSRAM)
uint8_t *pending = nullptr;  // the last flush, turned for the panel (PSRAM)
uint8_t *frame = nullptr;    // the one on the glass, or going there (PSRAM)
RefreshPlan plan;
uint32_t refreshes = 0;
uint32_t skipped = 0;  // flushes that changed no pixel
uint32_t last_changed = 0;  // pixels the last refresh changed
bool last_full = false;
uint32_t last_rotate_us = 0;
esp_err_t last_err = ESP_OK;

// The panel takes a second a frame: a task of its own draws the latest flush,
// so the ui task keeps reading the touch meanwhile (a tap is not lost). The
// panel's power stays on between frames (PIN taps come several a second) and
// goes off kPanelOffMs after the last one.
constexpr uint32_t kPanelOffMs = 2000;
constexpr uint32_t kPushStackBytes = 4096;
StackType_t push_stack[kPushStackBytes];
StaticTask_t push_storage;
TaskHandle_t push_task = nullptr;
SemaphoreHandle_t pending_mutex = nullptr;  // `pending` and the flags below
StaticSemaphore_t pending_storage;
SemaphoreHandle_t panel_mutex = nullptr;    // held for a refresh; HoldPanel waits on it
StaticSemaphore_t panel_storage;
bool have_pending = false;
bool frame_on_glass = false;  // `frame` is what the panel shows
volatile bool held = false;   // HoldPanel: frames stay off the panel

lv_indev_t *pointer = nullptr;
ScreenPoint last_point = {};  // LVGL wants a point on release too
bool last_down = false;
uint32_t presses = 0;
void (*press_hook)() = nullptr;
lv_obj_t *touch_mark = nullptr;
lv_obj_t *touch_label = nullptr;

uint32_t TickMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// DIRECT mode: LVGL redraws the dirty areas in place; the panel takes whole
// frames, so only the last area of a refresh goes out, to `pending`.
void Flush(lv_display_t *disp, const lv_area_t *, uint8_t *px_map) {
    if (lv_display_flush_is_last(disp)) {
        xSemaphoreTake(pending_mutex, portMAX_DELAY);
        const int64_t start = esp_timer_get_time();
        ToPanel(px_map + kPaletteBytes, pending);
        last_rotate_us = static_cast<uint32_t>(esp_timer_get_time() - start);
        have_pending = true;
        xSemaphoreGive(pending_mutex);
        xTaskNotifyGive(push_task);
    }
    lv_display_flush_ready(disp);
}

// Draws the latest pending frame; one that changes nothing on the glass is
// dropped (a pressed key that looks the same makes one).
void PushTask(void *) {
    bool panel_on = false;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, panel_on ? pdMS_TO_TICKS(kPanelOffMs) : portMAX_DELAY) == 0) {
            xSemaphoreTake(panel_mutex, portMAX_DELAY);
            epd::PowerOff();
            panel_on = false;
            xSemaphoreGive(panel_mutex);
            continue;
        }
        xSemaphoreTake(panel_mutex, portMAX_DELAY);
        xSemaphoreTake(pending_mutex, portMAX_DELAY);
        bool draw = have_pending && !held;
        uint32_t changed = 0;
        if (draw) {
            changed = frame_on_glass ? ChangedPixels(pending, frame, epd::kFrameBytes)
                                     : epd::kWidth * epd::kHeight;
            draw = changed != 0;
            skipped += draw ? 0 : 1;
            memcpy(frame, pending, epd::kFrameBytes);
            have_pending = false;
        }
        xSemaphoreGive(pending_mutex);
        if (draw) {
            last_full = plan.NextIsFull(changed);
            last_changed = changed;
            last_err = epd::Refresh(frame, last_full ? epd::Mode::kFull : epd::Mode::kFast, true);
            panel_on = last_err == ESP_OK;
            frame_on_glass = last_err == ESP_OK;
            ++refreshes;
            if (last_err != ESP_OK) {
                ESP_LOGE(TAG, "refresh: %s", esp_err_to_name(last_err));
            }
        }
        xSemaphoreGive(panel_mutex);
    }
}

// The GT911 as LVGL's pointer, read in the ui task (lv_timer_handler).
void ReadTouch(lv_indev_t *, lv_indev_data_t *data) {
    touch::gt911::Point p = {};
    const bool down = touch::Contact(&p);
    if (down) {
        last_point = TouchToScreen(p.x, p.y);
        if (!last_down) {
            ++presses;
            if (press_hook != nullptr) {
                press_hook();
            }
        }
    }
    last_down = down;
    data->point.x = last_point.x;
    data->point.y = last_point.y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Until the real screens: a cross where the finger went down and its
// coordinates (checks the touch mapping by eye).
void OnScreenPressed(lv_event_t *) {
    lv_point_t p = {};
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_set_pos(touch_mark, p.x - lv_obj_get_width(touch_mark) / 2,
                   p.y - lv_obj_get_height(touch_mark) / 2);
    lv_obj_remove_flag(touch_mark, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(touch_label, "touch %d, %d", static_cast<int>(p.x),
                          static_cast<int>(p.y));
}

void StartTouch() {
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, ReadTouch);
    lv_indev_set_display(pointer, display);
    lv_timer_set_period(lv_indev_get_read_timer(pointer), kTouchReadMs);

    lv_obj_t *scr = lv_screen_active();
    touch_mark = lv_label_create(scr);
    lv_label_set_text(touch_mark, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(touch_mark, &lv_font_montserrat_32, 0);
    lv_obj_add_flag(touch_mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(touch_mark);
    touch_label = lv_label_create(scr);
    lv_label_set_text(touch_label, "touch the screen");
    lv_obj_set_style_text_font(touch_label, &lv_font_montserrat_20, 0);
    lv_obj_align(touch_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(scr, OnScreenPressed, LV_EVENT_PRESSED, nullptr);
}

// Until the real screens: the name, the firmware and a 2 px frame inside the
// bezel (all of it shows when the insets are right).
void BootScreen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *border = lv_obj_create(scr);
    lv_obj_remove_style_all(border);
    lv_obj_remove_flag(border, LV_OBJ_FLAG_CLICKABLE);  // presses go to the screen
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
    pending = static_cast<uint8_t *>(heap_caps_malloc(epd::kFrameBytes, MALLOC_CAP_SPIRAM));
    frame = static_cast<uint8_t *>(heap_caps_malloc(epd::kFrameBytes, MALLOC_CAP_SPIRAM));
    if (screen == nullptr || pending == nullptr || frame == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    pending_mutex = xSemaphoreCreateMutexStatic(&pending_storage);
    panel_mutex = xSemaphoreCreateMutexStatic(&panel_storage);
    push_task = xTaskCreateStatic(&PushTask, "ui_push", kPushStackBytes, nullptr, kPriority,
                                  push_stack, &push_storage);
    if (push_task == nullptr) {
        return ESP_FAIL;
    }
    display = lv_display_create(kWidth, kHeight);
    if (display == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(display, screen, nullptr, kBufBytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, Flush);
    BootScreen();
    if (touch::Present()) {
        StartTouch();
        StartPinPad();  // typed on the touch: no touch, no pad (the console's `pin`)
    }
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

void SetPressHook(void (*hook)()) { press_hook = hook; }

void HoldPanel(bool hold) {
    if (push_task == nullptr) {
        return;
    }
    if (hold) {
        held = true;
        // A refresh under way finishes first.
        xSemaphoreTake(panel_mutex, portMAX_DELAY);
        xSemaphoreGive(panel_mutex);
        return;
    }
    xSemaphoreTake(pending_mutex, portMAX_DELAY);
    held = false;
    have_pending = false;    // the next LVGL change draws it all again
    frame_on_glass = false;  // the glass shows something else now
    xSemaphoreGive(pending_mutex);
}

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
    printf("ui: %ux%u portrait, %" PRIu32 " refreshes (full every %u or from %" PRIu32
           " px changed), %" PRIu32 " unchanged dropped, last %s %s, %" PRIu32
           " px, turn %" PRIu32 " us, stack free %u B (push %u B)%s\n",
           kWidth, kHeight, refreshes, RefreshPlan::kFullEvery, RefreshPlan::kBigChange, skipped,
           last_full ? "full" : "fast", esp_err_to_name(last_err), last_changed,
           last_rotate_us, static_cast<unsigned>(uxTaskGetStackHighWaterMark(task)),
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(push_task)),
           held ? ", panel held" : "");
    if (pointer == nullptr) {
        printf("ui: no touch\n");
        return;
    }
    Lock();
    const ScreenPoint p = last_point;
    const bool down = last_down;
    const uint32_t n = presses;
    Unlock();
    printf("ui: touch every %" PRIu32 " ms, %s at %u, %u, %" PRIu32 " presses\n", kTouchReadMs,
           down ? "down" : "up", p.x, p.y, n);
}

}  // namespace ui
