#include "ui.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "epd.h"
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

void StartPinPad();   // pin_pad.cpp
void StartScreens();  // screens.cpp
nav::Gesture TakeGesture();
bool LastWasTap();
ScreenPoint GestureStart();

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
ScreenPoint down_point = {};  // where the finger went down
nav::Gesture gesture = nav::Gesture::kTap;  // the last lift's path
bool gesture_new = false;                    // ... not taken by the screens yet
uint32_t presses = 0;
void (*press_hook)() = nullptr;

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

// The GT911 as LVGL's pointer, read in the ui task (lv_timer_handler): the
// edges the keypad task recorded, one per read (continue_reading for the
// rest), so a tap shorter than a busy ui task is still a press and a release.
// Between the edges the finger's place now: a swipe (nav::Classify, from the
// down point to the last place) is taken by the screens at the lift.
// Console `ui click` / `ui swipe`: a made-up finger, a step each read: down
// at `from`, `steps` moves to `to`, up.
struct Fake {
    ScreenPoint from;
    ScreenPoint to;
    uint16_t steps;
    uint16_t at;
    bool active;
};
Fake fake = {};

// The lift: what the finger's path was.
void Lifted() {
    gesture = nav::Classify(down_point.x, down_point.y, last_point.x, last_point.y, kHeight);
    gesture_new = gesture != nav::Gesture::kTap;
}

void Pressed(ScreenPoint p) {
    last_point = p;
    down_point = p;
    ++presses;
    if (press_hook != nullptr) {
        press_hook();
    }
}

// One step of the made-up finger.
void PlayFake() {
    if (fake.at == 0) {
        Pressed(fake.from);
        last_down = true;
    } else if (fake.at <= fake.steps) {
        const int32_t k = fake.at;
        const int32_t n = fake.steps;
        last_point.x = static_cast<uint16_t>(fake.from.x + (fake.to.x - fake.from.x) * k / n);
        last_point.y = static_cast<uint16_t>(fake.from.y + (fake.to.y - fake.from.y) * k / n);
    } else {
        Lifted();
        last_down = false;
        fake.active = false;
    }
    ++fake.at;
}

void ReadTouch(lv_indev_t *, lv_indev_data_t *data) {
    touch::gt911::Edge e = {};
    uint8_t more = 0;
    if (fake.active) {
        PlayFake();
    } else if (touch::NextEdge(&e, &more)) {
        if (e.down) {
            Pressed(TouchToScreen(e.x, e.y));
        } else {
            Lifted();
        }
        last_down = e.down;
        data->continue_reading = more > 0;
    } else if (last_down) {
        touch::gt911::Point p = {};
        if (touch::Contact(&p)) {
            last_point = TouchToScreen(p.x, p.y);
        }
    }
    const bool down = last_down;
    data->point.x = last_point.x;
    data->point.y = last_point.y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void StartTouch() {
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, ReadTouch);
    lv_indev_set_display(pointer, display);
    lv_timer_set_period(lv_indev_get_read_timer(pointer), kTouchReadMs);
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
    StartScreens();
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

// For the screens (ui task): the last lift's swipe, once.
nav::Gesture TakeGesture() {
    const bool fresh = gesture_new;
    gesture_new = false;
    return fresh ? gesture : nav::Gesture::kNone;
}

// For a click handler (ui task, right after the lift): the finger did not slide.
bool LastWasTap() { return gesture == nav::Gesture::kTap; }

// Where the last finger went down (a swipe's row).
ScreenPoint GestureStart() { return down_point; }

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

esp_err_t Touch(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t hold_ms) {
    if (pointer == nullptr) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (x0 >= kWidth || x1 >= kWidth || y0 >= kHeight || y1 >= kHeight) {
        return ESP_ERR_INVALID_ARG;
    }
    Lock();
    const bool busy = fake.active || last_down;
    if (!busy) {
        const bool tap = x0 == x1 && y0 == y1;
        const uint32_t held = hold_ms / kTouchReadMs;
        fake = {{x0, y0}, {x1, y1}, static_cast<uint16_t>(!tap ? 10 : held > 2 ? held : 2), 0,
                true};
    }
    Unlock();
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}

bool CopyScreen(uint8_t *out) {
    if (screen == nullptr) {
        return false;
    }
    Lock();
    memcpy(out, screen + kPaletteBytes, kScreenBytes);
    Unlock();
    return true;
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
