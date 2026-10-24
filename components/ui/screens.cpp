// The screens behind the PIN pad (x4-design.md): the status bar on LVGL's top
// layer (over every screen, the pad's too), the tab bar at the bottom, and
// between them the current screen's page. nav_logic.h and status_logic.h
// decide; this draws. The screens are stubs for now: a title, what comes
// there, and the page.

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "nav_logic.h"
#include "status_logic.h"
#include "ui.h"
#include "ui_logic.h"
#include "vault.h"

namespace ui {

namespace {

constexpr uint32_t kStatusMs = 2000;  // the bar's source, polled
constexpr uint32_t kInputMs = 30;     // the buttons' queue, drained
constexpr int32_t kBarHeight = 44;
constexpr int32_t kTabHeight = 72;
constexpr uint8_t kRowsPerPage = 8;  // the Passwords list (later)
constexpr unsigned kQueueDepth = 8;

constexpr int32_t kLeft = kInsetLeft;
constexpr int32_t kRight = kWidth - kInsetRight;
constexpr int32_t kContentTop = kInsetTop + kBarHeight;
constexpr int32_t kContentBottom = kHeight - kInsetBottom - kTabHeight;

nav::Nav nav_state;
QueueHandle_t inputs = nullptr;
StaticQueue_t inputs_storage;
uint8_t inputs_buffer[kQueueDepth];
void (*source)(status::Status *out) = nullptr;

lv_obj_t *bar_left = nullptr;
lv_obj_t *bar_right = nullptr;
char left_text[status::kTextSize] = "";
char right_text[status::kTextSize] = "";
int8_t shown_level = -1;

lv_obj_t *content = nullptr;
lv_obj_t *tabs = nullptr;
size_t shown_records = static_cast<size_t>(-1);

// The pad's map form: "" ends it.
const char *const kTabMap[] = {nav::kTitles[0], nav::kTitles[1], nav::kTitles[2],
                               nav::kTitles[3], ""};

lv_obj_t *Label(lv_obj_t *parent, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    return l;
}

// A label is set only when its text changed: every change is a refresh.
void SetIfChanged(lv_obj_t *label, char *kept, const char *text) {
    if (strcmp(kept, text) != 0) {
        snprintf(kept, status::kTextSize, "%s", text);
        lv_label_set_text(label, kept);
    }
}

void UpdateStatus() {
    if (source == nullptr || bar_left == nullptr) {
        return;
    }
    status::Status s;
    source(&s);
    shown_level = status::ShownLevel(shown_level, s.battery);
    s.battery = shown_level;
    char text[status::kTextSize];
    status::Left(s, text, sizeof text);
    SetIfChanged(bar_left, left_text, text);
    status::Right(s, text, sizeof text);
    SetIfChanged(bar_right, right_text, text);
}

void StartBar() {
    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bar, kLeft, kInsetTop);
    lv_obj_set_size(bar, kRight - kLeft, kBarHeight);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(bar, 12, 0);

    bar_left = Label(bar, &lv_font_montserrat_20);
    lv_label_set_text(bar_left, "");
    lv_obj_align(bar_left, LV_ALIGN_LEFT_MID, 0, 0);
    bar_right = Label(bar, &lv_font_montserrat_20);
    lv_label_set_text(bar_right, "");
    lv_obj_align(bar_right, LV_ALIGN_RIGHT_MID, 0, 0);
}

// Records in the vault (0 while it is locked) for the Passwords pages.
size_t Records() {
    const vault::Info info = vault::GetInfo();
    return info.state == vault::State::kOpen ? info.count : 0;
}

void Stub(const char *what) {
    lv_obj_t *l = Label(content, &lv_font_montserrat_20);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, kRight - kLeft - 48);
    lv_label_set_text(l, what);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 24, 80);
}

// The current screen's page, drawn anew.
void ShowPage() {
    lv_obj_clean(content);
    const nav::Screen s = nav_state.screen();
    lv_obj_t *title = Label(content, &lv_font_montserrat_32);
    lv_label_set_text(title, nav::Title(s));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 20);

    char text[160];
    switch (s) {
        case nav::Screen::kPasswords:
            snprintf(text, sizeof text,
                     "%u record(s).\n\nThe list comes here, %u a page: a tap on a row types the "
                     "login (left), the password (centre) or the TOTP code (right).",
                     static_cast<unsigned>(Records()), static_cast<unsigned>(kRowsPerPage));
            Stub(text);
            break;
        case nav::Screen::kTotp:
            Stub("The TOTP codes of the records come here.");
            break;
        case nav::Screen::kFido:
            Stub("The FIDO2 keys sites made on this device come here.");
            break;
        case nav::Screen::kSettings:
            snprintf(text, sizeof text,
                     "Frontlight, Wi-Fi and Bluetooth come here.\n\nfirmware %s\nLVGL %s",
                     esp_app_get_description()->version, LvglVersion());
            Stub(text);
            break;
    }

    if (nav_state.pages() > 1) {
        lv_obj_t *page = Label(content, &lv_font_montserrat_20);
        lv_label_set_text_fmt(page, LV_SYMBOL_LEFT "   %u / %u   " LV_SYMBOL_RIGHT,
                              static_cast<unsigned>(nav_state.page() + 1),
                              static_cast<unsigned>(nav_state.pages()));
        lv_obj_align(page, LV_ALIGN_BOTTOM_MID, 0, -16);
    }
    for (uint8_t i = 0; i < nav::kScreens; ++i) {
        if (i == static_cast<uint8_t>(s)) {
            lv_buttonmatrix_set_button_ctrl(tabs, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        } else {
            lv_buttonmatrix_clear_button_ctrl(tabs, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
    }
}

// The Passwords pages follow the vault (opened, a record added).
void FollowRecords() {
    const size_t n = Records();
    if (n == shown_records) {
        return;
    }
    shown_records = n;
    const bool moved = nav_state.SetPages(nav::Screen::kPasswords, nav::PagesFor(n, kRowsPerPage));
    if (nav_state.screen() == nav::Screen::kPasswords || moved) {
        ShowPage();
    }
}

void OnTab(lv_event_t *event) {
    auto *m = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const uint32_t id = lv_buttonmatrix_get_selected_button(m);
    if (id < nav::kScreens && nav_state.Go(static_cast<nav::Screen>(id))) {
        ShowPage();
    }
}

void StartTabs(lv_obj_t *scr) {
    tabs = lv_buttonmatrix_create(scr);
    lv_buttonmatrix_set_map(tabs, kTabMap);
    // The longer names wider: "Passwords" fills a quarter.
    const uint8_t kTabWidths[nav::kScreens] = {4, 3, 3, 4};
    for (uint8_t i = 0; i < nav::kScreens; ++i) {
        lv_buttonmatrix_set_button_width(tabs, i, kTabWidths[i]);
    }
    lv_obj_set_pos(tabs, kLeft, kContentBottom);
    lv_obj_set_size(tabs, kRight - kLeft, kTabHeight);
    lv_obj_set_style_bg_color(tabs, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(tabs, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(tabs, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(tabs, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(tabs, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tabs, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(tabs, 6, LV_PART_MAIN);
    // E-ink: no pressed look (as the PIN pad); the current tab inverted.
    const lv_style_selector_t kStates[] = {LV_STATE_DEFAULT, LV_STATE_PRESSED, LV_STATE_FOCUSED,
                                           LV_STATE_FOCUS_KEY};
    for (const lv_style_selector_t state : kStates) {
        const auto part = static_cast<lv_style_selector_t>(LV_PART_ITEMS) | state;
        lv_obj_set_style_bg_color(tabs, lv_color_white(), part);
        lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, part);
        lv_obj_set_style_border_width(tabs, 0, part);
        lv_obj_set_style_outline_width(tabs, 0, part);
        lv_obj_set_style_shadow_width(tabs, 0, part);
        lv_obj_set_style_radius(tabs, 10, part);
        lv_obj_set_style_text_color(tabs, lv_color_black(), part);
        lv_obj_set_style_text_font(tabs, &lv_font_montserrat_20, part);
    }
    const lv_style_selector_t kChecked[] = {LV_STATE_CHECKED,
                                            LV_STATE_CHECKED | LV_STATE_PRESSED,
                                            LV_STATE_CHECKED | LV_STATE_FOCUSED,
                                            LV_STATE_CHECKED | LV_STATE_FOCUS_KEY};
    for (const lv_style_selector_t state : kChecked) {
        const auto part = static_cast<lv_style_selector_t>(LV_PART_ITEMS) | state;
        lv_obj_set_style_bg_color(tabs, lv_color_black(), part);
        lv_obj_set_style_text_color(tabs, lv_color_white(), part);
    }
    lv_obj_set_style_outline_width(tabs, 0, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(tabs, OnTab, LV_EVENT_VALUE_CHANGED, nullptr);
}

// In the ui task: the buttons' inputs, the vault's records.
void Poll(lv_timer_t *) {
    uint8_t in = 0;
    bool changed = false;
    while (xQueueReceive(inputs, &in, 0) == pdTRUE) {
        // Under the PIN pad the buttons do nothing here.
        if (lv_screen_active() == lv_obj_get_screen(content)) {
            changed |= nav_state.Apply(static_cast<nav::Input>(in));
        }
    }
    if (changed) {
        ShowPage();
    }
    FollowRecords();
}

void StatusTimer(lv_timer_t *) { UpdateStatus(); }

}  // namespace

// From ui.cpp, under Lock: on the screen shown at boot.
void StartScreens() {
    inputs = xQueueCreateStatic(kQueueDepth, 1, inputs_buffer, &inputs_storage);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(content, kLeft, kContentTop);
    lv_obj_set_size(content, kRight - kLeft, kContentBottom - kContentTop);

    StartTabs(scr);
    StartBar();
    FollowRecords();
    ShowPage();
    UpdateStatus();
    lv_timer_create(Poll, kInputMs, nullptr);
    lv_timer_create(StatusTimer, kStatusMs, nullptr);
}

void SetStatusSource(void (*fn)(status::Status *out)) { source = fn; }

void Navigate(nav::Input input) {
    if (inputs != nullptr) {
        const auto in = static_cast<uint8_t>(input);
        xQueueSend(inputs, &in, 0);  // full: dropped (8 presses unread)
    }
}

}  // namespace ui
