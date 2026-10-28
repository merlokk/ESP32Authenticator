// The PIN pad (x4-design.md, Lock screen): shown while the vault is locked
// (at boot unless the key was kept through the sleep, after `pin lock`),
// gone once it opens, whoever opened it (the pad or the console `pin`).
// pin_pad_logic.h decides; this draws it and runs vault::Unlock off the ui
// task (the key derivation takes a second or more).

#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pin_pad_logic.h"
#include "ui.h"
#include "ui_logic.h"
#include "vault.h"

namespace ui {

namespace {

constexpr const char *TAG = "pin_pad";

constexpr uint32_t kWatchMs = 1000;            // the vault's state, polled
constexpr uint32_t kLockRefreshMs = 60 * 1000;  // the lock's minutes, redrawn
constexpr uint32_t kCheckStackBytes = 6144;    // as the console's unlock, and some
constexpr int32_t kDot = 28;
constexpr int32_t kDotGap = 20;

// The pad's keys (pin_pad_logic.h KeyAt): "" ends the map.
const char *const kMap[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
                            LV_SYMBOL_CLOSE, "0", LV_SYMBOL_BACKSPACE, ""};

pinpad::Pad pad;
lv_obj_t *pad_screen = nullptr;
lv_obj_t *home_screen = nullptr;  // what was shown before, back after the PIN
lv_obj_t *title = nullptr;
lv_obj_t *dots[pinpad::kDigits] = {};
lv_obj_t *message = nullptr;
bool new_store = false;
bool checking = false;  // a check task is running (written under ui::Lock)
uint32_t lock_drawn_ms = 0;
bool lock_shown = false;

char check_pin[pinpad::kDigits + 1];  // the check task's copy, wiped after

// A dot is a fast refresh (taps come several a second); the pad itself came
// in a full one (Open).
void ShowDots() {
    AskRefresh(Refresh::kFast);
    for (uint8_t i = 0; i < pinpad::kDigits; ++i) {
        lv_obj_set_style_bg_opa(dots[i], i < pad.count() ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

void ShowTitle() {
    const pinpad::Stage s = pad.stage();
    lv_label_set_text(title, s == pinpad::Stage::kNew      ? "New PIN"
                             : s == pinpad::Stage::kRepeat ? "Repeat the PIN"
                                                           : "Enter PIN");
}

// Under the dots: the lock, the tries left, or how it started.
void ShowState(const vault::Info &info, const char *first) {
    lock_shown = info.lock_left_s > 0;
    lock_drawn_ms = lv_tick_get();
    if (lock_shown) {
        lv_label_set_text_fmt(message, "too many wrong PINs: wait %u min",
                              static_cast<unsigned>(pinpad::MinutesUp(info.lock_left_s)));
    } else if (first != nullptr) {
        lv_label_set_text(message, first);
    } else if (info.pin_failures > 0) {
        lv_label_set_text_fmt(message, "%u tries left",
                              static_cast<unsigned>(pinpad::TriesLeft(info.pin_failures,
                                                                      vault::kMaxPinTries)));
    } else {
        lv_label_set_text(message, new_store ? "no store yet: this PIN makes its key" : "");
    }
}

void Close() {
    if (pad_screen == nullptr) {
        return;
    }
    pad.Wipe();
    lv_screen_load(home_screen);
    lv_obj_delete(pad_screen);
    pad_screen = nullptr;
    ESP_LOGI(TAG, "closed");
}

// After Unlock: right closes the pad (the watch would too), else why not.
void Checked(vault::Error e, const vault::Info &info) {
    if (e == vault::Error::kOk || info.state == vault::State::kOpen) {
        Close();
        return;
    }
    new_store = pinpad::NewStore(info.file_size, info.kept_version);
    pad.Done(new_store);
    ShowTitle();
    ShowDots();
    char why[64];
    const char *text = why;
    switch (e) {
        case vault::Error::kWrongPin:
            snprintf(why, sizeof why, "wrong PIN, %u tries left",
                     static_cast<unsigned>(pinpad::TriesLeft(info.pin_failures,
                                                             vault::kMaxPinTries)));
            break;
        case vault::Error::kPinLocked:
            text = nullptr;  // ShowState says how long
            break;
        case vault::Error::kWeakPin:
            text = "too simple: choose another PIN";
            break;
        default:
            snprintf(why, sizeof why, "%s", vault::Describe(e));
            break;
    }
    ShowState(info, text);
}

void CheckTask(void *) {
    const vault::Error e = vault::Unlock(check_pin);
    vault::Wipe(check_pin, sizeof check_pin);
    const vault::Info info = vault::GetInfo();
    ESP_LOGI(TAG, "%s", vault::Describe(e));
    Lock();
    checking = false;
    if (pad_screen != nullptr) {
        Checked(e, info);
    }
    Unlock();
    vTaskDelete(nullptr);
}

// The check's task, made when the PIN is complete. While the internal RAM is
// short (a sync's TLS session takes most of it for a few seconds) it is
// tried again every kRetryMs, kRetries times, the PIN kept meanwhile.
constexpr uint32_t kRetryMs = 500;
constexpr uint8_t kRetries = 40;
lv_timer_t *retry = nullptr;
uint8_t retries = 0;

bool StartCheck() {
    return xTaskCreate(&CheckTask, "pin_check", kCheckStackBytes, nullptr, 2, nullptr) == pdPASS;
}

void OnRetry(lv_timer_t *timer) {
    if (StartCheck()) {
        lv_timer_delete(timer);
        retry = nullptr;
        return;
    }
    if (++retries < kRetries) {
        return;
    }
    lv_timer_delete(timer);
    retry = nullptr;
    checking = false;
    vault::Wipe(check_pin, sizeof check_pin);
    pad.Done(new_store);
    if (pad_screen != nullptr) {
        lv_label_set_text(message, "no memory: try again");
        ShowTitle();
        ShowDots();
    }
}

void OnKey(lv_event_t *event) {
    auto *keys = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const uint32_t id = lv_buttonmatrix_get_selected_button(keys);
    const pinpad::Step step = pad.Apply(pinpad::KeyAt(id));
    switch (step) {
        case pinpad::Step::kNone:
            return;
        case pinpad::Step::kTyped:
            break;
        case pinpad::Step::kRepeat:
            lv_label_set_text(message, "");
            break;
        case pinpad::Step::kMismatch:
            lv_label_set_text(message, "the two PINs differ: a new one again");
            break;
        case pinpad::Step::kCheck:
            snprintf(check_pin, sizeof check_pin, "%s", pad.Pin());
            lv_label_set_text(message, "checking...");
            checking = true;
            if (!StartCheck() && retry == nullptr) {
                ESP_LOGW(TAG, "no memory for the check: trying again");
                retries = 0;
                retry = lv_timer_create(OnRetry, kRetryMs, nullptr);
            }
            break;
    }
    ShowTitle();
    ShowDots();
}

lv_obj_t *Label(lv_obj_t *parent, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    return l;
}

// E-ink: black on white, no pressed look (a press that changes nothing on
// the glass makes no refresh), keys outlined.
void Open() {
    const vault::Info info = vault::GetInfo();
    new_store = pinpad::NewStore(info.file_size, info.kept_version);
    pad.Start(new_store);
    home_screen = lv_screen_active();
    pad_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(pad_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pad_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(pad_screen, LV_OBJ_FLAG_SCROLLABLE);

    title = Label(pad_screen, &lv_font_montserrat_32);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kInsetTop + 70);

    const int32_t row = pinpad::kDigits * kDot + (pinpad::kDigits - 1) * kDotGap;
    for (uint8_t i = 0; i < pinpad::kDigits; ++i) {
        lv_obj_t *d = lv_obj_create(pad_screen);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, kDot, kDot);
        lv_obj_set_pos(d, (kWidth - row) / 2 + i * (kDot + kDotGap), kInsetTop + 160);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 3, 0);
        lv_obj_set_style_border_color(d, lv_color_black(), 0);
        lv_obj_set_style_bg_color(d, lv_color_black(), 0);
        dots[i] = d;
    }

    message = Label(pad_screen, &lv_font_montserrat_20);
    lv_obj_set_width(message, kWidth - kInsetLeft - kInsetRight - 40);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_TOP_MID, 0, kInsetTop + 215);

    lv_obj_t *keys = lv_buttonmatrix_create(pad_screen);
    lv_buttonmatrix_set_map(keys, kMap);
    lv_obj_set_size(keys, kWidth - kInsetLeft - kInsetRight - 40, 480);
    lv_obj_align(keys, LV_ALIGN_BOTTOM_MID, 0, -(kInsetBottom + 30));
    lv_obj_set_style_bg_opa(keys, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(keys, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(keys, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(keys, 16, LV_PART_MAIN);
    const lv_style_selector_t kStates[] = {LV_STATE_DEFAULT, LV_STATE_PRESSED, LV_STATE_FOCUSED,
                                           LV_STATE_FOCUS_KEY, LV_STATE_CHECKED};
    for (const lv_style_selector_t state : kStates) {
        const lv_style_selector_t part = static_cast<lv_style_selector_t>(LV_PART_ITEMS) | state;
        lv_obj_set_style_bg_color(keys, lv_color_white(), part);
        lv_obj_set_style_bg_opa(keys, LV_OPA_COVER, part);
        lv_obj_set_style_border_color(keys, lv_color_black(), part);
        lv_obj_set_style_border_width(keys, 2, part);
        lv_obj_set_style_outline_width(keys, 0, part);
        lv_obj_set_style_shadow_width(keys, 0, part);
        lv_obj_set_style_radius(keys, 12, part);
        lv_obj_set_style_text_color(keys, lv_color_black(), part);
        lv_obj_set_style_text_font(keys, &lv_font_montserrat_32, part);
    }
    lv_obj_set_style_outline_width(keys, 0, LV_STATE_FOCUSED);  // LV_PART_MAIN is 0
    lv_obj_add_event_cb(keys, OnKey, LV_EVENT_VALUE_CHANGED, nullptr);

    ShowTitle();
    ShowDots();
    ShowState(info, nullptr);
    AskRefresh(Refresh::kFull);
    lv_screen_load(pad_screen);
    ESP_LOGI(TAG, "shown (%s)", new_store ? "a new PIN" : "the PIN");
}

// Every second, in the ui task (lv_timer): the pad follows the vault's state.
void Watch(lv_timer_t *) {
    if (checking) {
        return;  // Unlock holds the vault; its answer comes to Checked
    }
    const vault::State state = vault::GetState();
    if (state == vault::State::kLocked && pad_screen == nullptr) {
        Open();
    } else if (state != vault::State::kLocked && pad_screen != nullptr) {
        Close();
    } else if (pad_screen != nullptr) {
        const vault::Info info = vault::GetInfo();
        const bool now_new = pinpad::NewStore(info.file_size, info.kept_version);
        if (now_new != new_store && pinpad::Untouched(pad.stage(), pad.count())) {
            // The store file came or went since the pad opened (the card reader).
            new_store = now_new;
            pad.Start(new_store);
            ShowTitle();
            ShowDots();
            ShowState(info, nullptr);
        } else if (lock_shown && lv_tick_elaps(lock_drawn_ms) >= kLockRefreshMs) {
            ShowState(info, nullptr);
        }
    }
}

}  // namespace

void StartPinPad() { lv_timer_create(Watch, kWatchMs, nullptr); }

}  // namespace ui
