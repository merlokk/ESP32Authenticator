// The screens behind the PIN pad (x4-design.md): the status bar on LVGL's top
// layer (over every screen, the pad's too), the password list in pages with a
// tab per page at the bottom, Settings pulled down from the top (the
// frontlight, then a row per page: SettingsHooks) and the U2F / FIDO2 keys pulled up from the bottom (a swipe left on a
// key: its trash can). nav_logic.h, list_logic.h, fido_list_logic.h and
// status_logic.h decide; this draws. Typing runs in a task of its own
// (ui::SetTypist; a password takes a second over BLE), and so does deleting a
// key (the store's file is written).

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "fido_ctap.h"
#include "fido_list_logic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "list_logic.h"
#include "lvgl.h"
#include "nav_logic.h"
#include "status_logic.h"
#include "codes.h"
#include "dialog.h"
#include "ui.h"
#include "ui_logic.h"
#include "vault.h"

namespace ui {

nav::Gesture TakeGesture();  // ui.cpp
bool LastWasTap();
ScreenPoint GestureStart();
ScreenPoint RawDown();
touch_cal::Calibration TouchCalibration();
void UseTouchCalibration(const touch_cal::Calibration &calibration);
esp_err_t SaveTouchCalibration(const touch_cal::Calibration &calibration);

namespace {

constexpr uint32_t kStatusMs = 500;   // the bar's source, polled (FIDO2's prompt)
constexpr uint32_t kPollMs = 30;      // buttons, swipes, the vault, the codes
constexpr uint32_t kToastMs = 3000;
constexpr uint32_t kVaultMs = 500;    // the vault's changes, looked at
constexpr uint32_t kFidoMs = 2000;    // the FIDO2 store's, while its list is shown
constexpr uint32_t kSettingsMs = 2000;  // Settings' texts, while shown
constexpr int32_t kBarHeight = 44;
constexpr int32_t kTabHeight = 72;
constexpr unsigned kQueueDepth = 8;
constexpr uint32_t kTypeStackBytes = 4096;
constexpr int64_t kTimeFloor = 1704067200;  // 2024-01-01, rtc::Believable's floor

constexpr int32_t kLeft = kInsetLeft;
constexpr int32_t kRight = kWidth - kInsetRight;
constexpr int32_t kRowWidth = kRight - kLeft;
constexpr int32_t kContentTop = kInsetTop + kBarHeight;
constexpr int32_t kContentBottom = kHeight - kInsetBottom - kTabHeight;
constexpr int32_t kScreenBottom = kHeight - kInsetBottom;  // no tabs (Settings, FIDO2)
// Over the rows, under the status bar: what a tap in each quarter does.
constexpr int32_t kHintHeight = 34;
constexpr int32_t kRowHeight = (kContentBottom - kContentTop - kHintHeight) / list::kRowsPerPage;
constexpr int32_t kQuarter = kRowWidth / 4;
// The rows' two lines: the site (and the TOTP code), the user and the password.
constexpr int32_t kSiteY = 10;
constexpr int32_t kSiteLine = 34;  // Montserrat 28: "..." cuts it there, no wrap
constexpr int32_t kUserY = 58;
constexpr int32_t kUserLine = 30;  // Montserrat 24
constexpr const char *kMask = "********";

nav::Nav nav_state;
QueueHandle_t inputs = nullptr;
StaticQueue_t inputs_storage;
uint8_t inputs_buffer[kQueueDepth];
void (*source)(status::Status *out) = nullptr;
esp_err_t (*typist)(const char *text) = nullptr;
const SettingsHooks *settings_hooks = nullptr;
uint32_t settings_looked_at = 0;

lv_obj_t *home = nullptr;
lv_obj_t *bar_left = nullptr;
lv_obj_t *bar_right = nullptr;
char left_text[status::kTextSize] = "";
char right_text[status::kTextSize] = "";
int8_t shown_level = -1;

lv_obj_t *content = nullptr;
lv_obj_t *tabs = nullptr;
lv_obj_t *toast = nullptr;
uint32_t toast_at = 0;
uint32_t looked_at = 0;
int tapped_tab = -1;  // OnTab: for Poll (the matrix is not changed in its own event)
bool back_tapped = false;  // OnBack: for Poll (the arrow is deleted with the page)
int open_mode = -1;        // ShowScreen (console): the nav::Mode to open, for Poll
bool fido_wait = false;    // a FIDO2 request waits for the button: its store is busy
char tab_text[nav::kTabs][16];
const char *tab_map[nav::kTabs + 1];

// What the list shows: redrawn when the vault changes (count, version, state).
struct Seen {
    vault::State state;
    size_t count;
    uint32_t version;
};
Seen seen = {vault::State::kOff, static_cast<size_t>(-1), 0};

struct Row {
    uint32_t id;
    bool has_otp;
    bool shown;  // the password is on the glass
    uint32_t shown_at;
    lv_obj_t *user;
    lv_obj_t *pass;
    lv_obj_t *code;
};
Row rows[list::kRowsPerPage];
uint8_t row_count = 0;
int64_t code_window = -2;
vault::Record record;  // the ui task's copy, wiped after each use

// The ui's own task: what may take a while (BLE typing, a file written).
enum class JobKind : uint8_t {
    kType,       // record `id`'s `zone`
    kDeleteKey,  // the FIDO2 key `key`
    kSaveLight,  // the frontlight's level into config.json
    kAct,        // a Settings page's `action`
    kWifiScan,
    kWifiActive,  // Wi-Fi on (`flag`) or off
    kWifiJoin,    // `ssid` with `password` (`flag`: the kept one)
    kWifiForget,  // `ssid`
    kBleActive,   // Bluetooth on (`flag`) or off
    kBleUse,      // the host `ssid` (an address) only; `flag`: any paired host
    kBleForget,   // the host `ssid`
    kBlePair,     // the pairing window opened (`flag`) or closed
    kBleName,     // the name `ssid` (`flag`), or the board's
    kSaveTouch,   // `calibration` into config.json
    kDisplaySet,  // Display & power: `setting` to `id`
    kTimeZone,    // the zone named in `password`
    kTimeSync,    // sync every `id` hours
    kTimeSet,     // the clock to `manual` (local)
    kPinChange,   // the PIN `ssid` to `password` (wiped after)
    kPinWipe,     // the wipe PIN `password` (`flag`), or none
    kPinLock,     // the lock after wrong PINs: `id` minutes
    kPinComplexity,  // `flag`
    kWebMode,     // web.mode `id`
    kWebWrite,    // web.write `flag`
    kWebLogin,    // the login `ssid` / `password` (`flag`), or none
    kWifiChoice,  // the Wi-Fi setting `which` (wifi_view::Choice) to `id`
    kWifiText,    // the Wi-Fi field `which` (wifi_view::Field) to `password`
    kWifiStatic,  // the network `ssid`'s address `static_ip`
};

struct Job {
    JobKind kind;
    uint32_t id;
    list::Zone zone;
    uint8_t key[FIDO_CTAP_CRED_ID_SIZE];
    settings::Action action;
    bool flag;
    char ssid[wifi_view::kSsidSize];
    char password[wifi_view::kPasswordSize];  // wiped after use
    touch_cal::Calibration calibration;
    display_view::Setting setting;
    time_view::Manual manual;
    uint8_t which;
    wifi_view::StaticIp static_ip;
};

// A job with only its kind set.
Job MakeJob(JobKind kind) {
    Job job = {};
    job.kind = kind;
    return job;
}
QueueHandle_t jobs = nullptr;
StaticQueue_t jobs_storage;
uint8_t jobs_buffer[4 * sizeof(Job)];
StackType_t type_stack[kTypeStackBytes];
StaticTask_t type_storage;
vault::Record type_record;  // the typing task's copy

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

// A label's text wiped in LVGL's heap before it is replaced (a password).
void SetWiped(lv_obj_t *label, const char *text) {
    char *old = lv_label_get_text(label);
    if (old != nullptr) {
        memset(old, 0, strlen(old));
    }
    lv_label_set_text(label, text);
}

bool HomeShown() { return lv_screen_active() == home; }

void Toast(const char *text) {
    lv_label_set_text(toast, text);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -(kInsetBottom + kTabHeight + 12));
    toast_at = lv_tick_get();
}

// --- status bar ------------------------------------------------------------------

// --- the FIDO2 prompt ----------------------------------------------------------------

fido_prompt::Request fido_request;  // SetFidoRequest, under Lock
lv_obj_t *prompt = nullptr;
bool prompt_demo = false;  // ui demo prompt
// The physical buttons, held upright: Left and Right on the edges at the
// password list's second row (about its height), Home under the glass.
constexpr int32_t kButtonsTop = 192;
constexpr int32_t kButtonsHeight = 106;

lv_obj_t *PromptLabel(lv_obj_t *parent, const lv_font_t *font, const char *text, int32_t y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, kRowWidth - 24);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// A hint by a button: black, the text white, against the screen's edge.
void PromptHint(lv_obj_t *parent, const char *text, lv_align_t align, int32_t y, int32_t width,
                int32_t height) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, width, height);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_align(b, align, 0, y);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
}

// Over everything (the top layer, under the status bar): what is asked, by
// whom, and the buttons' hints by the buttons.
void ShowPrompt() {
    const fido_prompt::Request &r = fido_request;
    prompt = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(prompt);
    lv_obj_add_flag(prompt, LV_OBJ_FLAG_CLICKABLE);  // the page under it takes no taps
    lv_obj_remove_flag(prompt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(prompt, kLeft, kContentTop);
    lv_obj_set_size(prompt, kRowWidth, kScreenBottom - kContentTop);
    lv_obj_set_style_bg_color(prompt, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(prompt, LV_OPA_COVER, 0);
    lv_obj_move_to_index(prompt, 0);  // under the status bar

    PromptLabel(prompt, &lv_font_montserrat_32, fido_prompt::Title(r.kind), 24);
    const int32_t hints = kButtonsTop - kContentTop;
    const bool reset = r.kind == fido_prompt::Kind::kReset;
    PromptHint(prompt, reset ? LV_SYMBOL_LEFT " Hold" : LV_SYMBOL_LEFT " Allow", LV_ALIGN_TOP_LEFT,
               hints, 170, kButtonsHeight);
    PromptHint(prompt, "Reject " LV_SYMBOL_RIGHT, LV_ALIGN_TOP_RIGHT, hints, 170, kButtonsHeight);

    char text[fido_prompt::kNames * fido_prompt::kTextSize + 128];
    int32_t y = hints + kButtonsHeight + 24;
    if (r.site[0] != '\0') {
        PromptLabel(prompt, &lv_font_montserrat_32, r.site, y);
        y += 90;
    }
    const int lines = fido_prompt::AccountText(r, text, sizeof text);
    if (lines > 0) {
        PromptLabel(prompt, &lv_font_montserrat_24, text, y);
        y += 30 * (lines < 2 ? 2 : lines) + 20;  // as two lines were before
    }
    const char *note = nullptr;
    switch (r.kind) {
        case fido_prompt::Kind::kRegister:
            note = r.resident ? "A passkey: kept on this device, with the account."
                              : "A key for this site.";
            break;
        case fido_prompt::Kind::kSignIn:
        case fido_prompt::Kind::kU2fSignIn:
            note = r.accounts == 0 ? "No key here for this site: Allow tells the site so."
                                   : "The site gets a signature from this device's key.";
            break;
        case fido_prompt::Kind::kU2fRegister: note = "A key for this site (U2F)."; break;
        case fido_prompt::Kind::kReset:
            note = "Hold Left 5 s: every key and the FIDO2 PIN are erased.";
            break;
        case fido_prompt::Kind::kSelect: note = "The host asks which key to use."; break;
        case fido_prompt::Kind::kOther: note = "A host asks for the button."; break;
    }
    snprintf(text, sizeof text, "%s%s", note, r.verify ? "\nThe PIN was given." : "");
    PromptLabel(prompt, &lv_font_montserrat_20, text, y);
    if (!reset) {
        PromptHint(prompt, LV_SYMBOL_DOWN " Home: allow", LV_ALIGN_BOTTOM_MID, -8, 260, 60);
    }
}

void HidePrompt() {
    if (prompt != nullptr) {
        lv_obj_delete(prompt);
        prompt = nullptr;
    }
}

void UpdateStatus(lv_timer_t *) {
    if (source == nullptr) {
        return;
    }
    status::Status s;
    source(&s);
    fido_wait = s.fido_wait;
    const bool want = fido_wait || prompt_demo;
    if (want && prompt == nullptr) {
        ShowPrompt();
    } else if (!want && prompt != nullptr) {
        HidePrompt();
    }
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
    lv_obj_set_size(bar, kRowWidth, kBarHeight);
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

    // Under the bar, over the tabs: what a tap did.
    toast = Label(lv_layer_top(), &lv_font_montserrat_24);
    lv_obj_set_style_bg_color(toast, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(toast, 2, 0);
    lv_obj_set_style_border_color(toast, lv_color_black(), 0);
    lv_obj_set_style_radius(toast, 10, 0);
    lv_obj_set_style_pad_all(toast, 12, 0);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

// --- the list ----------------------------------------------------------------------

// The state without the vault's lock (Unlock holds it for a second); the
// count and version only when open.
Seen Look() {
    const vault::State state = vault::GetState();
    if (state != vault::State::kOpen) {
        return {state, 0, 0};
    }
    const vault::Info info = vault::GetInfo();
    return {info.state, info.state == vault::State::kOpen ? info.count : 0, info.version};
}

bool TimeSet(int64_t now) { return now >= kTimeFloor; }

// The record behind a row into `record` (wipe it after); false if gone.
bool Fetch(uint32_t id, vault::Record *out) {
    const long pos = vault::Position(id);
    return pos >= 0 && vault::Get(static_cast<size_t>(pos), out) == vault::Error::kOk;
}

// The rows' TOTP codes, for the window `now` is in.
void UpdateCodes(int64_t now) {
    code_window = list::Window(now);
    for (uint8_t i = 0; i < row_count; ++i) {
        if (!rows[i].has_otp) {
            continue;
        }
        char text[8] = "--- ---";
        totp::Codes codes = {};
        if (TimeSet(now) && Fetch(rows[i].id, &record) &&
            totp::Generate(record.otp, now, &codes) == ESP_OK) {
            list::FormatCode(codes.code[1], text);
        }
        vault::Wipe(&record, sizeof record);
        if (strcmp(lv_label_get_text(rows[i].code), text) != 0) {
            lv_label_set_text(rows[i].code, text);
        }
    }
}

void HidePassword(Row &r) {
    SetWiped(r.pass, kMask);
    lv_obj_set_pos(r.pass, kRowWidth / 2 + 6, kUserY);
    lv_obj_set_size(r.pass, kRowWidth / 2 - 18, kUserLine);
    lv_obj_remove_flag(r.user, LV_OBJ_FLAG_HIDDEN);
    r.shown = false;
}

void ShowPassword(Row &r) {
    if (!Fetch(r.id, &record)) {
        Toast("the record is gone");
        return;
    }
    if (record.password[0] == '\0') {
        Toast("no password");
    } else {
        // Over the user's place: the whole width.
        lv_obj_add_flag(r.user, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(r.pass, 12, kUserY);
        lv_obj_set_size(r.pass, kRowWidth - 24, kUserLine);
        SetWiped(r.pass, record.password);
        r.shown = true;
        r.shown_at = lv_tick_get();
    }
    vault::Wipe(&record, sizeof record);
}

void OnRow(lv_event_t *event) {
    if (!LastWasTap()) {
        return;  // the finger slid: a swipe, not a tap
    }
    Row &r = rows[reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))];
    lv_point_t p = {};
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a = {};
    lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(event)), &a);
    const list::Zone zone = list::ZoneAt(p.x - a.x1, lv_area_get_width(&a));
    switch (list::OnTap(r.shown, zone)) {
        case list::Tap::kHide:
            HidePassword(r);
            return;
        case list::Tap::kShow:
            ShowPassword(r);
            return;
        case list::Tap::kType:
            break;
    }
    if (zone == list::Zone::kTypeTotp && !r.has_otp) {
        Toast("no TOTP for this record");
        return;
    }
    Job job = MakeJob(JobKind::kType);
    job.id = r.id;
    job.zone = zone;
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        Toast("busy typing");
    }
}

void AddRow(uint8_t i, const vault::Record &rec) {
    Row &r = rows[i];
    r = Row{};
    r.id = rec.id;
    r.has_otp = rec.otp[0] != '\0';

    lv_obj_t *row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(row, 0, kHintHeight + i * kRowHeight);
    lv_obj_set_size(row, kRowWidth, kRowHeight);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_add_event_cb(row, OnRow, LV_EVENT_CLICKED, reinterpret_cast<void *>(uintptr_t{i}));

    lv_obj_t *site = Label(row, &lv_font_montserrat_28);
    lv_label_set_long_mode(site, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(site, r.has_otp ? kRowWidth - 190 : kRowWidth - 24, kSiteLine);
    lv_label_set_text(site, rec.site);
    lv_obj_set_pos(site, 12, kSiteY);

    r.code = Label(row, &lv_font_montserrat_32);
    lv_label_set_text(r.code, "");
    lv_obj_align(r.code, LV_ALIGN_TOP_RIGHT, -12, kSiteY - 4);
    if (!r.has_otp) {
        lv_obj_add_flag(r.code, LV_OBJ_FLAG_HIDDEN);
    }

    r.user = Label(row, &lv_font_montserrat_24);
    lv_label_set_long_mode(r.user, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(r.user, kRowWidth / 2 - 18, kUserLine);
    lv_label_set_text(r.user, rec.user[0] != '\0' ? rec.user : "-");
    lv_obj_set_pos(r.user, 12, kUserY);

    r.pass = Label(row, &lv_font_montserrat_24);
    lv_label_set_long_mode(r.pass, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(r.pass, rec.password[0] != '\0' ? kMask : "-");
    lv_obj_set_pos(r.pass, kRowWidth / 2 + 6, kUserY);
    lv_obj_set_size(r.pass, kRowWidth / 2 - 18, kUserLine);
}

// One line over the rows: what a tap in each quarter of a row does.
void AddHints() {
    lv_obj_t *head = lv_obj_create(content);
    lv_obj_remove_style_all(head);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(head, kRowWidth, kHintHeight);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(head, 2, 0);
    lv_obj_set_style_border_color(head, lv_color_black(), 0);
    static const char *const kHints[4] = {LV_SYMBOL_KEYBOARD " user", LV_SYMBOL_KEYBOARD " pass",
                                          LV_SYMBOL_EYE_OPEN " show",
                                          LV_SYMBOL_KEYBOARD " TOTP"};
    for (uint8_t q = 0; q < 4; ++q) {
        lv_obj_t *hint = Label(head, &lv_font_montserrat_20);
        lv_label_set_text(hint, kHints[q]);
        lv_obj_set_width(hint, kQuarter);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(hint, q * kQuarter, 5);
        if (q > 0) {
            lv_obj_t *tick = lv_obj_create(head);
            lv_obj_remove_style_all(tick);
            lv_obj_set_style_bg_color(tick, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
            lv_obj_set_size(tick, 2, kHintHeight - 10);
            lv_obj_set_pos(tick, q * kQuarter, 4);
        }
    }
}

void Stub(const char *title, const char *what, int32_t top = 0) {
    if (title != nullptr) {
        lv_obj_t *t = Label(content, &lv_font_montserrat_32);
        lv_label_set_text(t, title);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 24, 20);
        top = 80;
    }
    lv_obj_t *l = Label(content, &lv_font_montserrat_24);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, kRowWidth - 48);
    lv_label_set_text(l, what);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 24, top);
}

void OnBack(lv_event_t *) {
    if (LastWasTap()) {
        back_tapped = true;
    }
}

// Settings / FIDO2: a back arrow and the screen's name over the page.
constexpr int32_t kTitleHeight = 56;

void TitleBar(const char *title, const char *right = nullptr) {
    lv_obj_t *head = lv_obj_create(content);
    lv_obj_remove_style_all(head);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(head, kRowWidth, kTitleHeight);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(head, 2, 0);
    lv_obj_set_style_border_color(head, lv_color_black(), 0);
    // The arrow's tap area: the left quarter, the bar's height.
    lv_obj_t *back = lv_obj_create(head);
    lv_obj_remove_style_all(back);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(back, kQuarter, kTitleHeight);
    lv_obj_add_event_cb(back, OnBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *arrow = Label(back, &lv_font_montserrat_28);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_align(arrow, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *name = Label(head, &lv_font_montserrat_28);
    lv_label_set_text(name, title);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 60, 0);
    if (right != nullptr) {
        lv_obj_t *r = Label(head, &lv_font_montserrat_24);
        lv_label_set_text(r, right);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, -12, 0);
    }
}

// --- the U2F / FIDO2 keys ----------------------------------------------------------

// Over the bar with the page arrows (the tabs' place).
constexpr int32_t kFidoRowHeight =
    (kContentBottom - kContentTop - kTitleHeight) / fido_list::kRowsPerPage;

// What the FIDO2 list shows: redrawn when the store changes.
struct FidoSeen {
    esp_err_t err;
    size_t count;
    uint32_t version;
};
FidoSeen fido_seen = {ESP_FAIL, 0, 0};
uint32_t fido_looked_at = 0;
// The store's credentials (~360 bytes each, up to 125: PSRAM) and their
// order by site; kept between showings.
fido_ctap_credential_t *fido_keys = nullptr;
uint16_t *fido_order = nullptr;
size_t fido_capacity = 0;
uint8_t fido_demo = 0;  // made-up keys added to the list (ui demo fido)
uint32_t demo_gone = 0;  // made-up keys deleted (a bit each)
// A made-up key's ID: this, then its number.
constexpr uint8_t kDemoId[] = {'d', 'e', 'm', 'o', 0xde, 0x40};

// Deleting a key (fido_list::Remove): the can on a row, then the question
// (dialog::Confirm, Yes held).
fido_list::Remove remove_state;
uint8_t fido_rows = 0;  // rows on the page
lv_obj_t *fido_trash[fido_list::kRowsPerPage] = {};
bool trash_tapped = false;  // OnTrash, OnFidoRow: for Poll
bool row_tapped = false;
// The held questions' answers (dialog ids; the choices' are 1..3).
constexpr int kDeleteNo = 100;
constexpr int kDeleteYes = 101;
constexpr int kResetNo = 200;
constexpr int kResetYes = 201;
fido_ctap_credential_t asked;  // the key asked about (the list may be read again)

// The made-up keys: site, user, display name, algorithm, kind.
struct Demo {
    const char *site;
    const char *user;
    const char *display;
    int32_t alg;
    bool u2f;
    bool discoverable;
};
constexpr Demo kDemo[] = {
    {"github.com", "octocat", "The Octocat", -7, false, true},
    {"google.com", "john.doe@gmail.com", "John Doe", -7, false, true},
    {"microsoft.com", "john@outlook.com", "", -7, false, true},
    {"login.microsoftonline.com", "john.doe@contoso.onmicrosoft.com", "John Doe", -7, false,
     true},
    {"amazon.com", "", "John", -7, false, true},
    {"U2F 3dec69ac", "", "", -7, true, false},
    {"dropbox.com", "jdoe", "", -7, false, false},
    {"accounts.firefox.com", "john.doe@mail.example", "", -8, false, true},
    {"gitlab.com", "jdoe", "John Doe", -8, false, true},
    {"cloudflare.com", "admin@example.org", "", -7, false, false},
};

// The made-up keys after the real ones in fido_keys (dated a day apart).
void AddDemo(size_t *count) {
    constexpr size_t kKinds = sizeof kDemo / sizeof kDemo[0];
    for (uint8_t i = 0; i < fido_demo && *count < fido_capacity; ++i) {
        if (demo_gone & (1u << i)) {
            continue;
        }
        const Demo &d = kDemo[i % kKinds];
        fido_ctap_credential_t &c = fido_keys[(*count)++];
        memset(&c, 0, sizeof c);
        memcpy(c.id, kDemoId, sizeof kDemoId);
        c.id[sizeof kDemoId] = i;
        if (i < kKinds) {
            snprintf(c.rp_id, sizeof c.rp_id, "%s", d.site);
        } else {
            snprintf(c.rp_id, sizeof c.rp_id, "%u.%s", static_cast<unsigned>(i / kKinds), d.site);
        }
        snprintf(c.user_name, sizeof c.user_name, "%s", d.user);
        snprintf(c.display_name, sizeof c.display_name, "%s", d.display);
        c.alg = d.alg;
        c.u2f = d.u2f;
        c.discoverable = d.discoverable;
        c.origin.created_utc = i % 4 == 3 ? 0 : 1767225600 + i * 86400LL;  // 2026-01-01 on
    }
}

FidoSeen FidoLook() {
    fido_ctap_info_t info;
    const esp_err_t err = fido_ctap_info(&info);
    return err == ESP_OK ? FidoSeen{err, info.credentials, info.file_version}
                         : FidoSeen{err, 0, 0};
}

// All the credentials into fido_keys, sorted by site, then user; `seen`:
// the store they came from.
esp_err_t FidoLoad(size_t *count, FidoSeen *seen) {
    *count = 0;
    fido_ctap_info_t info;
    esp_err_t err = fido_ctap_info(&info);
    *seen = {err, err == ESP_OK ? info.credentials : 0, err == ESP_OK ? info.file_version : 0};
    if (err != ESP_OK) {
        return err;
    }
    if (fido_capacity < info.max_credentials) {
        heap_caps_free(fido_keys);
        heap_caps_free(fido_order);
        fido_capacity = 0;
        const size_t n = info.max_credentials;
        const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        fido_keys = static_cast<fido_ctap_credential_t *>(
            heap_caps_malloc(n * sizeof(fido_ctap_credential_t), caps));
        fido_order = static_cast<uint16_t *>(heap_caps_malloc(n * sizeof(uint16_t), caps));
        if (fido_keys == nullptr || fido_order == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        fido_capacity = n;
    }
    err = fido_ctap_list(fido_keys, fido_capacity, count);
    if (err != ESP_OK) {
        return err;
    }
    AddDemo(count);
    fido_list::Sort(fido_order, *count, [](uint16_t a, uint16_t b) {
        const fido_ctap_credential_t &x = fido_keys[a];
        const fido_ctap_credential_t &y = fido_keys[b];
        const int site = fido_list::Compare(x.rp_id, y.rp_id);
        return site != 0 ? site < 0 : fido_list::Compare(x.user_name, y.user_name) < 0;
    });
    return ESP_OK;
}

void OnFidoRow(lv_event_t *) {
    if (LastWasTap()) {
        row_tapped = true;
    }
}

void OnTrash(lv_event_t *) {
    if (LastWasTap()) {
        trash_tapped = true;
    }
}

// The can at the row's right end (shown by ShowTrash), over the text.
constexpr int32_t kTrashWidth = 96;

void AddTrash(uint8_t i, lv_obj_t *row) {
    lv_obj_t *t = lv_obj_create(row);
    lv_obj_remove_style_all(t);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(t, kTrashWidth, kFidoRowHeight - 12);
    lv_obj_align(t, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(t, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, 10, 0);
    lv_obj_add_event_cb(t, OnTrash, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *icon = Label(t, &lv_font_montserrat_32);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_label_set_text(icon, LV_SYMBOL_TRASH);
    lv_obj_center(icon);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    fido_trash[i] = t;
}

// The can on remove_state's row, none on the others.
void ShowTrash() {
    for (uint8_t i = 0; i < fido_rows; ++i) {
        const bool on = remove_state.stage() != fido_list::Stage::kNone && remove_state.row() == i;
        if (on != !lv_obj_has_flag(fido_trash[i], LV_OBJ_FLAG_HIDDEN)) {
            if (on) {
                lv_obj_remove_flag(fido_trash[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(fido_trash[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

lv_obj_t *AskButton(lv_obj_t *parent, int32_t width, const char *text) {
    return dialog::FramedButton(parent, width, text);
}

// The can tapped: the key on its row asked about.
void AskRemove() {
    const size_t at =
        static_cast<size_t>(nav_state.fido_page()) * fido_list::kRowsPerPage + remove_state.row();
    if (remove_state.row() >= fido_rows || !remove_state.Trash()) {
        return;
    }
    asked = fido_keys[fido_order[at]];
    char site[FIDO_CTAP_TEXT_SIZE + 4];
    char user[2 * FIDO_CTAP_TEXT_SIZE + 4];
    snprintf(site, sizeof site, "%s%s", asked.rp_id, asked.rp_id_truncated ? "..." : "");
    fido_list::User(asked.user_name, asked.display_name, user, sizeof user);
    dialog::Confirm("Delete this key?", site, user,
                    "The site will not take this device for it any more: sign in another way "
                    "first.",
                    "Delete: hold 4 s", kDeleteNo, kDeleteYes);
}

// Yes held long enough: the key goes to the typing task to be deleted.
void DeleteAsked() {
    Job job = MakeJob(JobKind::kDeleteKey);
    memcpy(job.key, asked.id, sizeof job.key);
    Toast(xQueueSend(jobs, &job, 0) != pdTRUE ? "busy typing" : "deleting the key...");
    remove_state.Clear();
    ShowTrash();
}

void AddFidoRow(uint8_t i, const fido_ctap_credential_t &c) {
    lv_obj_t *row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, OnFidoRow, LV_EVENT_CLICKED, nullptr);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(row, 0, kTitleHeight + i * kFidoRowHeight);
    lv_obj_set_size(row, kRowWidth, kFidoRowHeight);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);

    char text[2 * FIDO_CTAP_TEXT_SIZE + 4];
    lv_obj_t *site = Label(row, &lv_font_montserrat_28);
    lv_label_set_long_mode(site, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(site, kRowWidth - 24, kSiteLine);
    snprintf(text, sizeof text, "%s%s", c.rp_id, c.rp_id_truncated ? "..." : "");
    lv_label_set_text(site, text);
    lv_obj_set_pos(site, 12, 6);

    lv_obj_t *user = Label(row, &lv_font_montserrat_24);
    lv_label_set_long_mode(user, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(user, kRowWidth - 24, kUserLine);
    fido_list::User(c.user_name, c.display_name, text, sizeof text);
    lv_label_set_text(user, text);
    lv_obj_set_pos(user, 12, 40);

    const bool dated = c.origin.created_utc > 0;
    struct tm local = {};
    if (dated) {
        const time_t when = static_cast<time_t>(c.origin.created_utc);
        localtime_r(&when, &local);
    }
    fido_list::Details(dated ? static_cast<uint16_t>(local.tm_year + 1900) : 0,
                       static_cast<uint8_t>(local.tm_mon + 1), static_cast<uint8_t>(local.tm_mday),
                       c.alg, c.u2f, c.discoverable, text, sizeof text);
    lv_obj_t *details = Label(row, &lv_font_montserrat_20);
    lv_label_set_text(details, text);
    lv_obj_set_pos(details, 12, 72);
    AddTrash(i, row);
}

// The title bar says how many keys; the bar under the list (ShowTabs) and
// Left / Right flip the pages.
void ShowFido() {
    size_t count = 0;
    // While a request waits for the button, the store's lock is taken.
    const esp_err_t err = fido_wait ? ESP_ERR_TIMEOUT : FidoLoad(&count, &fido_seen);
    nav_state.SetFidoPages(nav::PagesFor(count, fido_list::kRowsPerPage));
    char right[16] = "";
    if (err == ESP_OK && count > 0) {
        snprintf(right, sizeof right, "%u", static_cast<unsigned>(count));
    }
    TitleBar("U2F / FIDO2", right);
    const char *why = nullptr;
    if (err == ESP_ERR_TIMEOUT) {
        why = "Busy: a FIDO2 request is running.";
        fido_seen = {err, 0, 0};  // drawn again when it is done
    } else if (err == ESP_ERR_NO_MEM) {
        why = "No memory for the list.";
    } else if (err != ESP_OK) {
        why = fido_ctap_stopped_reason();
        why = why != nullptr ? why : "FIDO2 is not running.";
    } else if (count == 0) {
        why = "No keys yet: a site makes one when this device is registered as a security key "
              "or a passkey.";
    }
    if (why != nullptr) {
        Stub(nullptr, why, kTitleHeight + 24);
        return;
    }
    const size_t first = static_cast<size_t>(nav_state.fido_page()) * fido_list::kRowsPerPage;
    for (uint8_t i = 0; i < fido_list::kRowsPerPage && first + i < count; ++i) {
        AddFidoRow(i, fido_keys[fido_order[first + i]]);
        ++fido_rows;
    }
}

// --- Settings ----------------------------------------------------------------------

constexpr int32_t kLightRow = 64;
constexpr int32_t kMenuRow = 58;
constexpr int32_t kLineHeight = 50;
constexpr size_t kSettingsText = 512;
// What the shown page says (describe; the main page: its rows' summaries, a
// line each), looked at every kSettingsMs: drawn again when it changes.
char settings_shown[kSettingsText];
char settings_now[kSettingsText];
lv_obj_t *light_switch = nullptr;
lv_obj_t *light_value[2] = {};  // "60%" after the sliders
bool light_save = false;        // a slider let go: for Poll (the save job)
int settings_tapped = -1;       // a main page's row: its Page, for Poll
bool action_tapped = false;     // a page's button, for Poll
bool fido_reset_tapped = false;  // FIDO2's Reset: the held question

void SettingsText(settings::Page page, char *out, size_t size) {
    out[0] = '\0';
    if (settings_hooks == nullptr) {
        return;
    }
    if (page != settings::Page::kMain) {
        settings_hooks->describe(page, false, out, size);
        return;
    }
    size_t used = 0;
    for (const settings::Page p : settings::kRows) {
        settings_hooks->describe(p, true, out + used, size - used);
        used += strlen(out + used);
        if (used + 2 < size) {
            out[used++] = '\n';
            out[used] = '\0';
        }
    }
}

// A finger is on the glass (a slider dragged: no redraw under it).
bool FingerDown() {
    lv_indev_t *pointer = lv_indev_get_next(nullptr);
    return pointer != nullptr && lv_indev_get_state(pointer) == LV_INDEV_STATE_PRESSED;
}

void ShowPercent(uint8_t which, uint8_t value) {
    char text[8];
    snprintf(text, sizeof text, "%u%%", static_cast<unsigned>(value));
    lv_label_set_text(light_value[which], text);
}

void OnLightSwitch(lv_event_t *) {
    bool on = false;
    uint8_t brightness = 0;
    uint8_t warmth = 0;
    settings_hooks->light(&on, &brightness, &warmth);
    settings_hooks->set_light(lv_obj_has_state(light_switch, LV_STATE_CHECKED), brightness, warmth);
}

// Brightness (user data 0) or warmth (1): lit at once, saved when let go.
void OnLightSlider(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        light_save = true;
        return;
    }
    const auto which = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const int32_t v = lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(event)));
    bool on = false;
    uint8_t brightness = 0;
    uint8_t warmth = 0;
    settings_hooks->light(&on, &brightness, &warmth);
    if (which == 0) {
        brightness = settings::Brightness(v);
    } else {
        warmth = settings::Warmth(v);
    }
    settings_hooks->set_light(true, brightness, warmth);  // moved: lit
    lv_obj_add_state(light_switch, LV_STATE_CHECKED);
    ShowPercent(which, which == 0 ? brightness : warmth);
}

// E-ink: black and white, no animation.
lv_obj_t *Slider(lv_obj_t *parent, int32_t min, int32_t value) {
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_remove_style_all(s);
    lv_slider_set_range(s, min, 100);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_size(s, 250, 14);
    lv_obj_set_ext_click_area(s, 24);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(s, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 3, LV_PART_KNOB);
    lv_obj_set_style_border_color(s, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 9, LV_PART_KNOB);
    return s;
}

lv_obj_t *Switch(lv_obj_t *parent, bool on) {
    lv_obj_t *s = lv_switch_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 84, 44);
    lv_obj_set_style_anim_duration(s, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    const auto checked = static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED;
    lv_obj_set_style_bg_color(s, lv_color_black(), checked);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, checked);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(s, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, -5, LV_PART_KNOB);
    if (on) {
        lv_obj_add_state(s, LV_STATE_CHECKED);
    }
    return s;
}

// A plain box on the content: no style, no scrolling, not clickable.
lv_obj_t *Panel(lv_obj_t *parent, int32_t y, int32_t height) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(p, 0, y);
    lv_obj_set_size(p, kRowWidth, height);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(p, lv_color_black(), 0);
    return p;
}

// The frontlight: on / off, then brightness and warmth. Its height.
int32_t AddLight(int32_t y) {
    bool on = false;
    uint8_t level[2] = {};
    const bool has = settings_hooks->light(&on, &level[0], &level[1]);
    lv_obj_t *block = Panel(content, y, has ? 3 * kLightRow : kLightRow);
    lv_obj_set_style_border_width(block, 2, 0);
    lv_obj_t *name = Label(block, &lv_font_montserrat_28);
    lv_label_set_text(name, "Frontlight");
    lv_obj_set_pos(name, 12, (kLightRow - 34) / 2);
    if (!has) {
        lv_obj_t *none = Label(block, &lv_font_montserrat_20);
        lv_label_set_text(none, "none on this board");
        lv_obj_align(none, LV_ALIGN_TOP_RIGHT, -12, (kLightRow - 24) / 2);
        return kLightRow;
    }
    light_switch = Switch(block, on);
    lv_obj_align(light_switch, LV_ALIGN_TOP_RIGHT, -12, (kLightRow - 44) / 2);
    lv_obj_add_event_cb(light_switch, OnLightSwitch, LV_EVENT_VALUE_CHANGED, nullptr);
    static const char *const kNames[2] = {"Level", "Warmth"};
    for (uint8_t i = 0; i < 2; ++i) {
        const int32_t row = kLightRow * (1 + i);
        lv_obj_t *l = Label(block, &lv_font_montserrat_20);
        lv_label_set_text(l, kNames[i]);
        lv_obj_set_pos(l, 12, row + (kLightRow - 24) / 2);
        lv_obj_t *s = Slider(block, i == 0 ? 1 : 0, level[i]);
        lv_obj_set_pos(s, 112, row + (kLightRow - 14) / 2);
        void *which = reinterpret_cast<void *>(uintptr_t{i});
        lv_obj_add_event_cb(s, OnLightSlider, LV_EVENT_VALUE_CHANGED, which);
        lv_obj_add_event_cb(s, OnLightSlider, LV_EVENT_RELEASED, which);
        light_value[i] = Label(block, &lv_font_montserrat_20);
        lv_obj_set_style_text_align(light_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(light_value[i], 70);
        lv_obj_set_pos(light_value[i], kRowWidth - 82, row + (kLightRow - 24) / 2);
        ShowPercent(i, level[i]);
    }
    return 3 * kLightRow;
}

void OnSettingsRow(lv_event_t *event) {
    if (LastWasTap()) {
        settings_tapped = static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    }
}

void OnAction(lv_event_t *) {
    if (LastWasTap()) {
        action_tapped = true;
    }
}

// The frontlight, then a row per page: its name, its summary, ">".
void ShowSettingsMain() {
    TitleBar("Settings");
    int32_t y = kTitleHeight + AddLight(kTitleHeight) + 12;
    char summary[48];
    char unused[2];
    for (uint8_t i = 0; i < settings::kRowCount; ++i, y += kMenuRow) {
        lv_obj_t *row = Panel(content, y, kMenuRow);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_add_event_cb(row, OnSettingsRow, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(settings::kRows[i])));
        lv_obj_t *name = Label(row, &lv_font_montserrat_28);
        lv_label_set_text(name, settings::Title(settings::kRows[i]));
        lv_obj_set_pos(name, 12, (kMenuRow - 34) / 2);
        lv_obj_t *more = Label(row, &lv_font_montserrat_24);
        lv_label_set_text(more, LV_SYMBOL_RIGHT);
        lv_obj_align(more, LV_ALIGN_RIGHT_MID, -12, 0);
        if (!settings::Line(settings_shown, i, summary, sizeof summary, unused, sizeof unused)) {
            continue;
        }
        lv_obj_t *s = Label(row, &lv_font_montserrat_20);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(s, 190, 26);
        lv_label_set_text(s, summary);
        lv_obj_align(s, LV_ALIGN_RIGHT_MID, -44, 0);
    }
}

// A page: "label  value" lines, its button at the bottom.
void ShowSettingsPage(settings::Page page) {
    TitleBar(settings::Title(page));
    const settings::Action action = settings::PageAction(page);
    const int32_t bottom = kScreenBottom - kContentTop - (action != settings::Action::kNone ? 100 : 0);
    char label[32];
    char value[64];
    int32_t y = kTitleHeight + 8;
    for (uint8_t i = 0; y + kLineHeight <= bottom &&
                        settings::Line(settings_shown, i, label, sizeof label, value, sizeof value);
         ++i, y += kLineHeight) {
        lv_obj_t *l = Label(content, &lv_font_montserrat_24);
        lv_label_set_text(l, label);
        lv_obj_set_pos(l, 12, y + (kLineHeight - 30) / 2);
        lv_obj_t *v = Label(content, &lv_font_montserrat_24);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(v, 270, 30);
        lv_label_set_text(v, value);
        lv_obj_set_pos(v, kRowWidth - 282, y + (kLineHeight - 30) / 2);
    }
    if (action != settings::Action::kNone) {
        lv_obj_t *b = AskButton(content, kRowWidth - 48, settings::ActionText(action));
        lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_add_event_cb(b, OnAction, LV_EVENT_CLICKED, nullptr);
    }
}

// --- Settings: Wi-Fi ---------------------------------------------------------------

constexpr int32_t kNetRow = 64;
constexpr size_t kScanCapacity = 20;  // raw results, tidied to kMaxNearby
const WifiHooks *wifi_hooks = nullptr;
wifi_view::View wifi_shown;
wifi_view::View wifi_now;
// The scan: written by the ui's own task under Lock; drawn when its count changes.
wifi_view::Nearby nearby[kScanCapacity];
size_t nearby_count = 0;
bool scanning = false;
uint32_t scans = 0;
uint32_t scans_shown = 0;
char join_ssid[wifi_view::kSsidSize];  // the password page's network
lv_obj_t *password_area = nullptr;
int8_t sheet_index = wifi_view::kNone;
// Taps, for Poll.
bool wifi_switch_tapped = false;
int8_t saved_tapped = wifi_view::kNone;
int8_t nearby_tapped = wifi_view::kNone;
bool add_tapped = false;
bool rescan_tapped = false;
int sheet_choice = 0;  // a dialog's answer (dialog::Choose ids), for the pages' Poll
bool password_ready = false;
bool password_cancel = false;

wifi_view::Choice wifi_choice = wifi_view::Choice::kMode;  // the choice page's
wifi_view::Field wifi_field = wifi_view::Field::kApName;    // the typed page's
wifi_view::StaticIp static_edit;  // the IP page's copy, saved whole
char static_ssid[wifi_view::kSsidSize];  // whose
char hidden_ssid[wifi_view::kSsidSize];  // the hidden network's name, typed first
bool hidden_on_password = false;
int more_row_tapped = -1;    // 0 mode, 1 name, 2 password, 3 channel, 4 rounds, 5 window, 6 hostname
int choice_tapped = -1;
int static_row_tapped = -1;  // 0 the switch, 1..4 address, netmask, gateway, DNS
bool static_save = false;
bool more_tapped = false;
bool hidden_tapped = false;

void OnMore(lv_event_t *) {
    if (LastWasTap()) {
        more_tapped = true;
    }
}

void OnMoreRow(lv_event_t *event) {
    if (LastWasTap()) {
        more_row_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnWifiChoice(lv_event_t *event) {
    if (LastWasTap()) {
        choice_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnStaticRow(lv_event_t *event) {
    static_row_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
}

void OnStaticSave(lv_event_t *) {
    if (LastWasTap()) {
        static_save = true;
    }
}

void OnHidden(lv_event_t *) {
    if (LastWasTap()) {
        hidden_tapped = true;
    }
}

// The ui's own task: a scan (seconds), then the list.
void WifiScanJob() {
    static wifi_view::Nearby found[kScanCapacity];  // off the task's stack
    size_t n = wifi_hooks->scan(found, kScanCapacity);
    n = wifi_view::Tidy(found, n, wifi_view::kMaxNearby);
    Lock();
    memcpy(nearby, found, n * sizeof found[0]);
    nearby_count = n;
    scanning = false;
    ++scans;
    if (n == 0) {
        Toast("no networks found");
    }
    Unlock();
}

void StartScan() {
    if (scanning) {
        return;
    }
    const Job job = MakeJob(JobKind::kWifiScan);
    scanning = xQueueSend(jobs, &job, 0) == pdTRUE;
    if (!scanning) {
        Toast("busy typing");
    }
}

// A job that may carry a password or PINs: sent, then this copy wiped.
void SendAndWipe(Job &job) {
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        Toast("busy typing");
    }
    vault::Wipe(&job, sizeof job);
}

void OnWifiSwitch(lv_event_t *) { wifi_switch_tapped = true; }

void OnSaved(lv_event_t *event) {
    if (LastWasTap()) {
        saved_tapped = static_cast<int8_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnNearby(lv_event_t *event) {
    if (LastWasTap()) {
        nearby_tapped = static_cast<int8_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnAdd(lv_event_t *) {
    if (LastWasTap()) {
        add_tapped = true;
    }
}

void OnRescan(lv_event_t *) {
    if (LastWasTap()) {
        rescan_tapped = true;
    }
}

void OnPassword(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_READY) {
        password_ready = true;
    } else {
        password_cancel = true;
    }
}

// Signal bars (Bars of 4), bottom aligned, at x, y (their top).
void AddBars(lv_obj_t *parent, int32_t x, int32_t y, uint8_t bars) {
    for (uint8_t i = 0; i < 4; ++i) {
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_remove_style_all(b);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        const int32_t h = 6 + 4 * i;
        lv_obj_set_size(b, 6, h);
        lv_obj_set_pos(b, x + 9 * i, y + 18 - h);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_black(), 0);
        lv_obj_set_style_bg_color(b, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(b, i < bars ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

// A padlock, 16 x 20, at x, y.
void AddLock(lv_obj_t *parent, int32_t x, int32_t y) {
    lv_obj_t *shackle = lv_obj_create(parent);
    lv_obj_remove_style_all(shackle);
    lv_obj_remove_flag(shackle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(shackle, 12, 14);
    lv_obj_set_pos(shackle, x + 2, y);
    lv_obj_set_style_border_width(shackle, 2, 0);
    lv_obj_set_style_border_color(shackle, lv_color_black(), 0);
    lv_obj_set_style_radius(shackle, 6, 0);
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_remove_style_all(body);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(body, 16, 12);
    lv_obj_set_pos(body, x, y + 8);
    lv_obj_set_style_bg_color(body, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 2, 0);
}

lv_obj_t *SectionHead(int32_t y, const char *text) {
    lv_obj_t *l = Label(content, &lv_font_montserrat_20);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, 12, y);
    return l;
}

// A row: its name, its value and ">" (no value: a check, or nothing), tapped as `id`.
lv_obj_t *ChoiceRow(int32_t y, const char *name, const char *value, bool check,
                    lv_event_cb_t cb, int id) {
    lv_obj_t *row = Panel(content, y, kNetRow);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(id)));
    lv_obj_t *n = Label(row, &lv_font_montserrat_24);
    lv_label_set_text(n, name);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 12, 0);  // stays in the middle when the row is made lower
    if (value != nullptr) {
        lv_obj_t *v = Label(row, &lv_font_montserrat_20);
        lv_label_set_text(v, value);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -44, 0);
    }
    lv_obj_t *mark = Label(row, &lv_font_montserrat_24);
    lv_label_set_text(mark, value != nullptr ? LV_SYMBOL_RIGHT : check ? LV_SYMBOL_OK : "");
    lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -12, 0);
    return row;
}

// The switch, the state, the saved networks, "Add a network".
void ShowWifi() {
    TitleBar("Wi-Fi");
    wifi_hooks->view(&wifi_shown);
    const wifi_view::View &v = wifi_shown;
    int32_t y = kTitleHeight;
    lv_obj_t *top = Panel(content, y, kLightRow);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_t *name = Label(top, &lv_font_montserrat_28);
    lv_label_set_text(name, "Wi-Fi");
    lv_obj_set_pos(name, 12, (kLightRow - 34) / 2);
    lv_obj_t *sw = Switch(top, v.active);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -12, (kLightRow - 44) / 2);
    lv_obj_add_event_cb(sw, OnWifiSwitch, LV_EVENT_VALUE_CHANGED, nullptr);
    y += kLightRow;

    lv_obj_t *state = Panel(content, y, 52);
    lv_obj_set_style_border_width(state, 2, 0);
    lv_obj_t *s = Label(state, &lv_font_montserrat_24);
    lv_label_set_text(s, v.state);
    lv_obj_set_pos(s, 12, 10);
    lv_obj_t *d = Label(state, &lv_font_montserrat_20);
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(d, 260, 26);
    lv_label_set_text(d, v.detail);
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, -12, 0);
    y += 52;
    char mode[48];
    snprintf(mode, sizeof mode, "%s", wifi_view::ModeLabel(v.mode));
    ChoiceRow(y, "Mode and access point", mode, false, OnMore, 0);
    y += kNetRow + 16;

    SectionHead(y, "SAVED NETWORKS");
    y += 32;
    if (v.saved_count == 0) {
        lv_obj_t *none = Label(content, &lv_font_montserrat_24);
        lv_label_set_text(none, "none yet");
        lv_obj_set_pos(none, 12, y + 14);
        y += kNetRow;
    }
    for (uint8_t i = 0; i < v.saved_count && i < wifi_view::kMaxSaved; ++i, y += kNetRow) {
        lv_obj_t *row = Panel(content, y, kNetRow);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_add_event_cb(row, OnSaved, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t{i}));
        lv_obj_t *ssid = Label(row, &lv_font_montserrat_24);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(ssid, 250, 30);
        lv_label_set_text(ssid, v.saved[i].ssid);
        lv_obj_set_pos(ssid, 12, (kNetRow - 30) / 2);
        const char *note = v.joined == i         ? "connected"
                           : v.saved[i].refused ? "wrong password"
                                                : "";
        lv_obj_t *n = Label(row, &lv_font_montserrat_20);
        lv_label_set_text(n, note);
        lv_obj_align(n, LV_ALIGN_RIGHT_MID, -44, 0);
        lv_obj_t *more = Label(row, &lv_font_montserrat_24);
        lv_label_set_text(more, LV_SYMBOL_RIGHT);
        lv_obj_align(more, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    if (v.saved_count < wifi_view::kMaxSaved) {
        lv_obj_t *add = Panel(content, y, kNetRow);
        lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(add, 1, 0);
        lv_obj_add_event_cb(add, OnAdd, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *l = Label(add, &lv_font_montserrat_24);
        lv_label_set_text(l, LV_SYMBOL_PLUS "  Add a network");
        lv_obj_set_pos(l, 12, (kNetRow - 30) / 2);
    } else {
        lv_obj_t *full = Label(content, &lv_font_montserrat_20);
        lv_label_set_text(full, "4 saved at most: forget one to add another.");
        lv_obj_set_pos(full, 12, y + 12);
    }
}

// The networks nearby, strongest first; "Scan again".
void ShowWifiScan() {
    TitleBar("Add a network");
    scans_shown = scans;
    int32_t y = kTitleHeight;
    if (nearby_count == 0) {
        Stub(nullptr, scanning ? "Looking for networks..." : "No networks found.", y + 24);
    }
    for (uint8_t i = 0; i < nearby_count; ++i, y += kNetRow) {
        const wifi_view::Nearby &n = nearby[i];
        lv_obj_t *row = Panel(content, y, kNetRow);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_add_event_cb(row, OnNearby, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t{i}));
        lv_obj_t *ssid = Label(row, &lv_font_montserrat_24);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(ssid, 300, 30);
        lv_label_set_text(ssid, n.ssid);
        lv_obj_set_pos(ssid, 12, (kNetRow - 30) / 2);
        if (wifi_view::SavedIndex(wifi_shown, n.ssid) != wifi_view::kNone) {
            lv_obj_t *saved = Label(row, &lv_font_montserrat_20);
            lv_label_set_text(saved, "saved");
            lv_obj_align(saved, LV_ALIGN_RIGHT_MID, -100, 0);
        }
        if (n.secured) {
            AddLock(row, kRowWidth - 82, (kNetRow - 20) / 2);
        }
        AddBars(row, kRowWidth - 50, (kNetRow - 18) / 2, wifi_view::Bars(n.rssi));
    }
    lv_obj_t *other = AskButton(content, 180, "Other...");
    lv_obj_align(other, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_add_event_cb(other, OnHidden, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *b = AskButton(content, kRowWidth - 228, scanning ? "Scanning..." : "Scan again");
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_add_event_cb(b, OnRescan, LV_EVENT_CLICKED, nullptr);
}

// E-ink keys: white, framed; the mode keys inverted; no pressed look.
void StyleKeyboard(lv_obj_t *kb) {
    lv_obj_set_style_bg_color(kb, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(kb, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(kb, 5, LV_PART_MAIN);
    const lv_style_selector_t kStates[] = {LV_STATE_DEFAULT, LV_STATE_PRESSED, LV_STATE_FOCUSED,
                                           LV_STATE_FOCUS_KEY};
    for (const lv_style_selector_t state : kStates) {
        const auto part = static_cast<lv_style_selector_t>(LV_PART_ITEMS) | state;
        lv_obj_set_style_bg_color(kb, lv_color_white(), part);
        lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, part);
        lv_obj_set_style_border_width(kb, 2, part);
        lv_obj_set_style_border_color(kb, lv_color_black(), part);
        lv_obj_set_style_outline_width(kb, 0, part);
        lv_obj_set_style_shadow_width(kb, 0, part);
        lv_obj_set_style_radius(kb, 6, part);
        lv_obj_set_style_text_color(kb, lv_color_black(), part);
        lv_obj_set_style_text_font(kb, &lv_font_montserrat_24, part);
        const auto checked = part | LV_STATE_CHECKED;
        lv_obj_set_style_bg_color(kb, lv_color_black(), checked);
        lv_obj_set_style_text_color(kb, lv_color_white(), checked);
    }
    lv_obj_set_style_outline_width(kb, 0, LV_STATE_FOCUSED);
}

// A line typed on the keyboard (password_area, wiped when the page goes):
// `initial` in it, hidden as * when `masked`; OK and the keyboard key set
// password_ready / password_cancel (the page's Poll takes them).
lv_obj_t *KeyboardPage(const char *title, const char *placeholder, const char *initial, bool masked,
                  uint32_t max_length, const char *hint_text) {
    TitleBar(title);
    password_area = lv_textarea_create(content);
    lv_obj_remove_style_all(password_area);
    lv_textarea_set_one_line(password_area, true);
    lv_textarea_set_password_mode(password_area, masked);
    lv_textarea_set_password_show_time(password_area, 0);
    lv_textarea_set_password_bullet(password_area, "*");  // the font's bullet is a dot
    lv_textarea_set_max_length(password_area, max_length);
    // No placeholder: on one bit it looks typed (the hint under says what goes in).
    (void)placeholder;
    lv_textarea_set_text(password_area, initial);
    lv_obj_set_size(password_area, kRowWidth - 24, 60);
    lv_obj_set_pos(password_area, 12, kTitleHeight + 16);
    lv_obj_set_style_text_font(password_area, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(password_area, lv_color_black(), 0);
    lv_obj_set_style_border_width(password_area, 2, 0);
    lv_obj_set_style_border_color(password_area, lv_color_black(), 0);
    lv_obj_set_style_radius(password_area, 8, 0);
    lv_obj_set_style_pad_hor(password_area, 12, 0);
    lv_obj_set_style_pad_ver(password_area, 12, 0);
    // A steady cursor: no blinking refreshes.
    lv_obj_set_style_anim_duration(password_area, 0, LV_PART_CURSOR);
    lv_obj_set_style_border_side(password_area, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_width(password_area, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_color(password_area, lv_color_black(), LV_PART_CURSOR);
    lv_obj_add_state(password_area, LV_STATE_FOCUSED);

    lv_obj_t *hint = Label(content, &lv_font_montserrat_20);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(hint, kRowWidth - 24);
    lv_label_set_text(hint, hint_text);
    lv_obj_set_pos(hint, 12, kTitleHeight + 90);

    lv_obj_t *kb = lv_keyboard_create(content);
    StyleKeyboard(kb);
    lv_obj_set_size(kb, kRowWidth, 440);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, password_area);
    lv_obj_add_event_cb(kb, OnPassword, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, OnPassword, LV_EVENT_CANCEL, nullptr);
    return kb;
}

// The password on the keyboard: its OK joins, its keyboard key goes back.
void ShowWifiPassword() {
    KeyboardPage(join_ssid, "password", "", true, wifi_view::kPasswordSize - 1,
                 "8..63 characters. " LV_SYMBOL_OK ": join, " LV_SYMBOL_KEYBOARD ": back.");
}

// The typed password out of LVGL's heap.
void WipePassword() {
    if (password_area != nullptr) {
        auto *text = const_cast<char *>(lv_textarea_get_text(password_area));
        if (text != nullptr) {
            memset(text, 0, strlen(text));
        }
        password_area = nullptr;
    }
}

using SheetButton = dialog::Button;

void CloseSheet() { dialog::Close(); }

void OpenSheet(const char *title, const SheetButton *buttons, size_t count) {
    dialog::Choose(title, buttons, count);
}

// A saved network: Connect (when not on it), Forget, Cancel.
void ShowSheet(int8_t index) {
    sheet_index = index;
    const SheetButton all[] = {
        {"Connect", 1}, {"IP address", 4}, {"Forget this network", 2}, {"Cancel", 3}};
    const bool joined = wifi_shown.joined == index;
    OpenSheet(wifi_shown.saved[index].ssid, joined ? all + 1 : all, joined ? 3 : 4);
}

// The Wi-Fi window's taps (Poll). True when the page is to be drawn again.
bool PollWifi() {
    bool changed = false;
    if (wifi_switch_tapped) {
        wifi_switch_tapped = false;
        Job job = MakeJob(JobKind::kWifiActive);
        job.flag = !wifi_shown.active;
        SendAndWipe(job);
    }
    if (saved_tapped != wifi_view::kNone) {
        if (!dialog::IsOpen() && saved_tapped < wifi_shown.saved_count) {
            ShowSheet(saved_tapped);
        }
        saved_tapped = wifi_view::kNone;
    }
    if (sheet_choice != 0 && nav_state.settings_page() == static_cast<uint8_t>(settings::Page::kWifi)) {
        const int choice = sheet_choice;
        sheet_choice = 0;
        const bool ours = sheet_index >= 0 && sheet_index < wifi_shown.saved_count;
        if (choice == 4 && ours) {
            snprintf(static_ssid, sizeof static_ssid, "%s", wifi_shown.saved[sheet_index].ssid);
            static_edit = {};
            wifi_hooks->get_static(static_cast<uint8_t>(sheet_index), &static_edit);
            changed |= nav_state.OpenSettings(static_cast<uint8_t>(settings::Page::kWifiStatic));
        } else if ((choice == 1 || choice == 2) && ours) {
            Job job = MakeJob(choice == 1 ? JobKind::kWifiJoin : JobKind::kWifiForget);
            job.flag = true;  // the kept password
            snprintf(job.ssid, sizeof job.ssid, "%s", wifi_shown.saved[sheet_index].ssid);
            SendAndWipe(job);
        }
        CloseSheet();
    }
    if (add_tapped) {
        add_tapped = false;
        nearby_count = 0;
        StartScan();
        changed |= nav_state.OpenSettings(static_cast<uint8_t>(settings::Page::kWifiScan));
    }
    if (rescan_tapped) {
        rescan_tapped = false;
        if (!scanning) {
            StartScan();
            changed = true;  // "Scanning..."
        }
    }
    if (nearby_tapped != wifi_view::kNone) {
        const int8_t i = nearby_tapped;
        nearby_tapped = wifi_view::kNone;
        if (i < static_cast<int8_t>(nearby_count)) {
            const wifi_view::Nearby &n = nearby[i];
            const wifi_view::Join how = wifi_view::OnTap(wifi_shown, n);
            if (how == wifi_view::Join::kPassword) {
                snprintf(join_ssid, sizeof join_ssid, "%s", n.ssid);
                changed |= nav_state.OpenSettings(static_cast<uint8_t>(settings::Page::kWifiPassword));
            } else {
                Job job = MakeJob(JobKind::kWifiJoin);
                job.flag = how == wifi_view::Join::kSaved;
                snprintf(job.ssid, sizeof job.ssid, "%s", n.ssid);
                SendAndWipe(job);
                changed |= nav_state.OpenSettings(static_cast<uint8_t>(settings::Page::kWifi));
            }
        }
    }
    if (nav_state.settings_page() != static_cast<uint8_t>(settings::Page::kWifiPassword)) {
        return changed;
    }
    if (password_cancel) {
        password_cancel = false;
        changed |= nav_state.Back();
    }
    if (password_ready) {
        password_ready = false;
        if (password_area != nullptr) {
            const char *typed = lv_textarea_get_text(password_area);
            if (!wifi_view::PasswordOk(typed)) {
                Toast("8..63 characters, or 64 hex digits");
            } else {
                Job job = MakeJob(JobKind::kWifiJoin);
                snprintf(job.ssid, sizeof job.ssid, "%s", join_ssid);
                snprintf(job.password, sizeof job.password, "%s", typed);
                SendAndWipe(job);
                changed |= nav_state.OpenSettings(static_cast<uint8_t>(settings::Page::kWifi));
            }
        }
    }
    const auto page = static_cast<settings::Page>(nav_state.settings_page());
    if (page == settings::Page::kWifiScan && scans != scans_shown) {
        changed = true;
    }
    return changed;
}

// --- Settings: Bluetooth -----------------------------------------------------------

constexpr uint8_t kHostRows = 6;  // on the page; more are on the console
const BleHooks *ble_hooks = nullptr;
ble_view::View ble_shown;
ble_view::View ble_now;
ble_view::PairPage pair_page;
bool pair_open = false;  // the pairing window was opened from here
uint32_t pair_left_shown = 0;
bool ble_switch_tapped = false;
int8_t host_tapped = -1;
int8_t host_index = -1;  // the dialog's host
bool pair_tapped = false;
bool pair_button = false;  // Stop / Open again / Done
bool ble_name_tapped = false;
constexpr int kNameChange = 11;  // the name dialog's answers
constexpr int kNameBoard = 12;
constexpr int kNameCancel = 13;

void SendJob(Job &job) {
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        Toast("busy typing");
    }
}

void OnBleSwitch(lv_event_t *) { ble_switch_tapped = true; }

void OnBleName(lv_event_t *) {
    if (LastWasTap()) {
        ble_name_tapped = true;
    }
}

void OnHost(lv_event_t *event) {
    if (LastWasTap()) {
        host_tapped = static_cast<int8_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnPair(lv_event_t *) {
    if (LastWasTap()) {
        pair_tapped = true;
    }
}

void OnPairButton(lv_event_t *) {
    if (LastWasTap()) {
        pair_button = true;
    }
}

// The switch, the state and name, the paired hosts, "Pair a new host".
void ShowBluetooth() {
    TitleBar("Bluetooth");
    ble_hooks->view(&ble_shown);
    const ble_view::View &v = ble_shown;
    int32_t y = kTitleHeight;
    lv_obj_t *top = Panel(content, y, kLightRow);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_t *name = Label(top, &lv_font_montserrat_28);
    lv_label_set_text(name, "Bluetooth");
    lv_obj_set_pos(name, 12, (kLightRow - 34) / 2);
    lv_obj_t *sw = Switch(top, v.on);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -12, (kLightRow - 44) / 2);
    lv_obj_add_event_cb(sw, OnBleSwitch, LV_EVENT_VALUE_CHANGED, nullptr);
    y += kLightRow;

    lv_obj_t *state = Panel(content, y, 52);
    lv_obj_set_style_border_width(state, 2, 0);
    lv_obj_t *s = Label(state, &lv_font_montserrat_24);
    lv_label_set_text(s, v.state);
    lv_obj_set_pos(s, 12, 10);
    y += 52;
    ChoiceRow(y, "Name", v.name, false, OnBleName, 0);
    y += kNetRow + 16;
    if (!v.on) {
        Stub(nullptr, "Bluetooth is off: no typing, no FIDO2 keys over the air.", y);
        return;
    }

    SectionHead(y, "PAIRED HOSTS");
    y += 32;
    if (v.host_count == 0) {
        lv_obj_t *none = Label(content, &lv_font_montserrat_24);
        lv_label_set_text(none, "none yet");
        lv_obj_set_pos(none, 12, y + 14);
        y += kNetRow;
    }
    for (uint8_t i = 0; i < v.host_count && i < kHostRows; ++i, y += kNetRow) {
        lv_obj_t *row = Panel(content, y, kNetRow);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_add_event_cb(row, OnHost, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t{i}));
        lv_obj_t *addr = Label(row, &lv_font_montserrat_24);
        lv_label_set_text(addr, v.hosts[i].addr);
        lv_obj_set_pos(addr, 12, (kNetRow - 30) / 2);
        const char *note = v.hosts[i].connected ? "connected" : v.hosts[i].target ? "only one" : "";
        lv_obj_t *n = Label(row, &lv_font_montserrat_20);
        lv_label_set_text(n, note);
        lv_obj_align(n, LV_ALIGN_RIGHT_MID, -44, 0);
        lv_obj_t *more = Label(row, &lv_font_montserrat_24);
        lv_label_set_text(more, LV_SYMBOL_RIGHT);
        lv_obj_align(more, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    lv_obj_t *add = Panel(content, y, kNetRow);
    lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(add, 1, 0);
    lv_obj_add_event_cb(add, OnPair, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = Label(add, &lv_font_montserrat_24);
    lv_label_set_text(l, LV_SYMBOL_PLUS "  Pair a new host");
    lv_obj_set_pos(l, 12, (kNetRow - 30) / 2);
}

// The pairing window: what to do on the host, the passkey it asks for.
void ShowBlePair() {
    TitleBar("Pair a new host");
    char text[160];
    int32_t y = kTitleHeight + 24;
    const ble_view::Pairing state = pair_page.state();
    if (state == ble_view::Pairing::kPasskey) {
        Stub(nullptr, "Type this number on the host:", y);
        char key[12];
        ble_view::Passkey(pair_page.passkey(), key, sizeof key);
        lv_obj_t *k = Label(content, &lv_font_montserrat_48);
        lv_label_set_text(k, key);
        lv_obj_align(k, LV_ALIGN_TOP_MID, 0, y + 80);
        y += 180;
    } else {
        const char *what = state == ble_view::Pairing::kPaired ? "Paired."
                           : state == ble_view::Pairing::kClosed
                               ? "The pairing window has closed."
                               : nullptr;
        if (what == nullptr) {
            snprintf(text, sizeof text,
                     "%sOn the host: Bluetooth settings, add a device, then \"%s\".",
                     state == ble_view::Pairing::kFailed ? "That did not work: try again.\n\n" : "",
                     ble_shown.name);
            what = text;
        }
        Stub(nullptr, what, y);
        y += 200;
    }
    if (state != ble_view::Pairing::kPaired && state != ble_view::Pairing::kClosed) {
        snprintf(text, sizeof text, "Open for %u s more.", static_cast<unsigned>(pair_left_shown));
        Stub(nullptr, text, y);
    }
    const char *button = state == ble_view::Pairing::kPaired   ? "Done"
                         : state == ble_view::Pairing::kClosed ? "Open it again"
                                                               : "Stop";
    lv_obj_t *b = AskButton(content, kRowWidth - 48, button);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(b, OnPairButton, LV_EVENT_CLICKED, nullptr);
}

void OpenPairing() {
    pair_page.Start();
    pair_left_shown = ble_view::kPairSeconds;
    pair_open = true;
    Job job = MakeJob(JobKind::kBlePair);
    job.flag = true;
    SendJob(job);
}

void ClosePairing() {
    if (pair_open) {
        pair_open = false;
        Job job = MakeJob(JobKind::kBlePair);
        job.flag = false;
        SendJob(job);
    }
}

// The Bluetooth window's taps and the pairing window (Poll). True when the
// page is to be drawn again.
bool PollBle() {
    bool changed = false;
    const auto page = static_cast<settings::Page>(nav_state.settings_page());
    if (ble_switch_tapped) {
        ble_switch_tapped = false;
        Job job = MakeJob(JobKind::kBleActive);
        job.flag = !ble_shown.on;
        SendJob(job);
    }
    if (host_tapped >= 0) {
        if (!dialog::IsOpen() && host_tapped < ble_shown.host_count) {
            host_index = host_tapped;
            const ble_view::Host &h = ble_shown.hosts[host_index];
            const SheetButton buttons[] = {
                {h.target ? "Let any paired host in" : "Use only this host", 1},
                {"Forget this host", 2},
                {"Cancel", 3}};
            OpenSheet(h.addr, buttons, 3);
        }
        host_tapped = -1;
    }
    if (sheet_choice >= 1 && sheet_choice <= 3 && page == settings::Page::kBluetooth) {  // a host's
        const int choice = sheet_choice;
        sheet_choice = 0;
        if (choice != 3 && host_index >= 0 && host_index < ble_shown.host_count) {
            const ble_view::Host &h = ble_shown.hosts[host_index];
            Job job = MakeJob(choice == 1 ? JobKind::kBleUse : JobKind::kBleForget);
            job.flag = choice == 1 && h.target;  // any host again
            snprintf(job.ssid, sizeof job.ssid, "%s", h.addr);
            SendJob(job);
        }
        CloseSheet();
    }
    if (ble_name_tapped) {
        ble_name_tapped = false;
        if (!dialog::IsOpen()) {
            const dialog::Button buttons[] = {{"Change the name", kNameChange},
                                              {"Use the board's name", kNameBoard},
                                              {"Cancel", kNameCancel}};
            dialog::Choose("The name hosts see", buttons, 3);
        }
    }
    if (sheet_choice >= kNameChange && sheet_choice <= kNameCancel &&
        page == settings::Page::kBluetooth) {
        const int choice = sheet_choice;
        sheet_choice = 0;
        if (choice == kNameChange) {
            changed |= nav_state.OpenSettings(static_cast<uint8_t>(settings::Page::kBleName));
        } else if (choice == kNameBoard) {
            Job job = MakeJob(JobKind::kBleName);
            job.flag = false;  // the board's
            SendJob(job);
        }
    }
    if (page == settings::Page::kBleName) {
        if (password_cancel) {
            password_cancel = false;
            changed |= nav_state.Back();
        }
        if (password_ready && password_area != nullptr) {
            password_ready = false;
            const char *typed = lv_textarea_get_text(password_area);
            if (!ble_view::NameOk(typed)) {
                Toast("1..29 characters");
            } else {
                Job job = MakeJob(JobKind::kBleName);
                job.flag = true;
                snprintf(job.ssid, sizeof job.ssid, "%s", typed);
                snprintf(ble_shown.name, sizeof ble_shown.name, "%s", typed);
                SendJob(job);
                changed |= nav_state.Back();
            }
        }
    }
    if (pair_tapped) {
        pair_tapped = false;
        OpenPairing();
        changed |= nav_state.OpenSettings(static_cast<uint8_t>(settings::Page::kBlePair));
    }
    if (page != settings::Page::kBlePair) {
        return changed;
    }
    if (pair_button) {
        pair_button = false;
        if (pair_page.state() == ble_view::Pairing::kClosed) {
            OpenPairing();
            return true;
        }
        ClosePairing();
        return nav_state.Back() || changed;
    }
    ble_view::Pairing what = ble_view::Pairing::kWaiting;
    uint32_t key = 0;
    while (ble_hooks->pairing_event(&what, &key)) {
        changed |= what == ble_view::Pairing::kPasskey ? pair_page.Passkey(key)
                   : what == ble_view::Pairing::kPaired ? pair_page.Paired()
                                                        : pair_page.Failed();
    }
    if (pair_page.state() == ble_view::Pairing::kPaired) {
        pair_open = false;  // the window closes on a new bond
    }
    // The time left in steps of 10 s: a refresh each, not each second.
    const uint32_t left = pair_open ? ble_hooks->pairing_left() : 0;
    if (pair_open && left == 0 && pair_page.Left(0)) {
        pair_open = false;
        changed = true;
    }
    const uint32_t step = (left + 9) / 10 * 10;
    if (pair_open && step != pair_left_shown && pair_page.state() != ble_view::Pairing::kPaired) {
        pair_left_shown = step;
        changed = true;
    }
    return changed;
}

// --- Settings: Wi-Fi's mode and access point, a network's IP, a hidden network ---

const char *ChoiceLabel(wifi_view::Choice c, uint16_t value, char *out, size_t size) {
    const int i = wifi_view::ChoiceIndex(c, value);
    size_t n = 0;
    const wifi_view::Option *o = wifi_view::Options(c, &n);
    if (c == wifi_view::Choice::kMode) {
        snprintf(out, size, "%s", wifi_view::ModeLabel(static_cast<uint8_t>(value)));
    } else if (i >= 0) {
        snprintf(out, size, "%s", o[i].label);
    } else {
        snprintf(out, size, "%u", static_cast<unsigned>(value));
    }
    return out;
}

uint16_t ChoiceValue(wifi_view::Choice c) {
    const wifi_view::View &v = wifi_shown;
    return c == wifi_view::Choice::kMode      ? v.mode
           : c == wifi_view::Choice::kChannel ? v.channel
           : c == wifi_view::Choice::kRounds  ? v.rounds
                                              : v.window;
}

// The mode, the access point (name, password, channel, when it opens, how
// long it stays), the hostname.
void ShowWifiMore() {
    TitleBar("Mode and access point");
    wifi_hooks->view(&wifi_shown);
    const wifi_view::View &v = wifi_shown;
    char text[48];
    int32_t y = kTitleHeight;
    ChoiceRow(y, "Mode", ChoiceLabel(wifi_view::Choice::kMode, v.mode, text, sizeof text), false,
              OnMoreRow, 0);
    y += kNetRow + 12;
    SectionHead(y, "ACCESS POINT");
    y += 32;
    ChoiceRow(y, "Name", v.ap_name, false, OnMoreRow, 1);
    y += kNetRow;
    ChoiceRow(y, "Password", v.ap_password ? "set" : "open", false, OnMoreRow, 2);
    y += kNetRow;
    ChoiceRow(y, "Channel", ChoiceLabel(wifi_view::Choice::kChannel, v.channel, text, sizeof text),
              false, OnMoreRow, 3);
    y += kNetRow;
    ChoiceRow(y, "Opens", ChoiceLabel(wifi_view::Choice::kRounds, v.rounds, text, sizeof text),
              false, OnMoreRow, 4);
    y += kNetRow;
    ChoiceRow(y, "Stays", ChoiceLabel(wifi_view::Choice::kWindow, v.window, text, sizeof text),
              false, OnMoreRow, 5);
    y += kNetRow + 12;
    ChoiceRow(y, "Hostname", v.hostname, false, OnMoreRow, 6);
    y += kNetRow;
    lv_obj_t *note = Label(content, &lv_font_montserrat_20);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(note, kRowWidth - 24);
    lv_label_set_text(note, v.mode == wifi_view::kModeClient
                                ? "Join a network: the access point opens only when no saved "
                                  "network answers, and closes again."
                                : "The access point is always up.");
    lv_obj_set_pos(note, 12, y + 10);
}

void ShowWifiChoice() {
    TitleBar(wifi_view::ChoiceTitle(wifi_choice));
    size_t n = 0;
    const wifi_view::Option *o = wifi_view::Options(wifi_choice, &n);
    const int in_use = wifi_view::ChoiceIndex(wifi_choice, ChoiceValue(wifi_choice));
    // 13 channels: two columns would be tighter; a row each, 52 px.
    const int32_t row = n > 9 ? 52 : kNetRow;
    int32_t y = kTitleHeight;
    for (size_t i = 0; i < n; ++i, y += row) {
        lv_obj_t *r = ChoiceRow(y, o[i].label, nullptr, static_cast<int>(i) == in_use, OnWifiChoice,
                                static_cast<int>(i));
        lv_obj_set_height(r, row);
    }
}

// A typed field (the access point's, the hostname, an address, a hidden
// network's): its keyboard, digits first for the addresses.
void ShowWifiText(wifi_view::Field f, const char *initial) {
    const wifi_view::FieldSpec spec = wifi_view::Spec(f);
    char hint[160];
    snprintf(hint, sizeof hint, "%s " LV_SYMBOL_OK ": save, " LV_SYMBOL_KEYBOARD ": back.",
             spec.hint);
    lv_obj_t *kb = KeyboardPage(spec.title, spec.placeholder, initial, spec.masked, spec.max, hint);
    if (spec.numeric) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    }
}

// A saved network's address: DHCP or static, the fields, Save.
void ShowWifiStatic() {
    TitleBar("IP address");
    int32_t y = kTitleHeight;
    lv_obj_t *who = Label(content, &lv_font_montserrat_24);
    lv_label_set_long_mode(who, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(who, kRowWidth - 24);
    lv_label_set_text(who, static_ssid);
    lv_obj_set_pos(who, 12, y + 12);
    y += 52;
    lv_obj_t *row = Panel(content, y, kNetRow);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_t *l = Label(row, &lv_font_montserrat_24);
    lv_label_set_text(l, "Static address");
    lv_obj_set_pos(l, 12, (kNetRow - 30) / 2);
    lv_obj_t *sw = Switch(row, static_edit.enabled);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(sw, OnStaticRow, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void *>(intptr_t{0}));
    y += kNetRow;
    if (static_edit.enabled) {
        const wifi_view::Field fields[4] = {wifi_view::Field::kAddress, wifi_view::Field::kNetmask,
                                            wifi_view::Field::kGateway, wifi_view::Field::kDns};
        for (uint8_t i = 0; i < 4; ++i, y += kNetRow) {
            const char *value = wifi_view::StaticField(static_edit, fields[i]);
            ChoiceRow(y, wifi_view::Spec(fields[i]).title, value[0] != '\0' ? value : "not set",
                      false, OnStaticRow, i + 1);
        }
    } else {
        lv_obj_t *note = Label(content, &lv_font_montserrat_20);
        lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(note, kRowWidth - 24);
        lv_label_set_text(note, "The router gives the address (DHCP).");
        lv_obj_set_pos(note, 12, y + 12);
    }
    lv_obj_t *b = AskButton(content, kRowWidth - 48, "Save");
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(b, OnStaticSave, LV_EVENT_CLICKED, nullptr);
}

// Mode and access point, the IP page and the hidden network (Poll). True
// when the page is to be drawn again.
bool PollWifiMore() {
    bool changed = false;
    const auto page = static_cast<settings::Page>(nav_state.settings_page());
    auto open = [&](settings::Page p) {
        changed |= nav_state.OpenSettings(static_cast<uint8_t>(p));
    };
    if (more_tapped) {
        more_tapped = false;
        open(settings::Page::kWifiMore);
    }
    if (more_row_tapped >= 0) {
        const int row = more_row_tapped;
        more_row_tapped = -1;
        if (row == 0 || (row >= 3 && row <= 5)) {
            wifi_choice = row == 0   ? wifi_view::Choice::kMode
                          : row == 3 ? wifi_view::Choice::kChannel
                          : row == 4 ? wifi_view::Choice::kRounds
                                     : wifi_view::Choice::kWindow;
            open(settings::Page::kWifiChoice);
        } else {
            wifi_field = row == 1   ? wifi_view::Field::kApName
                         : row == 2 ? wifi_view::Field::kApPassword
                                    : wifi_view::Field::kHostname;
            open(settings::Page::kWifiText);
        }
    }
    if (choice_tapped >= 0) {
        size_t n = 0;
        const wifi_view::Option *o = wifi_view::Options(wifi_choice, &n);
        if (static_cast<size_t>(choice_tapped) < n) {
            Job job = MakeJob(JobKind::kWifiChoice);
            job.which = static_cast<uint8_t>(wifi_choice);
            job.id = o[choice_tapped].value;
            SendJob(job);
            changed |= nav_state.Back();
        }
        choice_tapped = -1;
    }
    if (hidden_tapped) {
        hidden_tapped = false;
        hidden_on_password = false;
        hidden_ssid[0] = '\0';
        open(settings::Page::kWifiHidden);
    }
    if (static_row_tapped >= 0 && page == settings::Page::kWifiStatic) {
        const int row = static_row_tapped;
        static_row_tapped = -1;
        if (row == 0) {
            static_edit.enabled = !static_edit.enabled;
            changed = true;
        } else {
            const wifi_view::Field fields[4] = {wifi_view::Field::kAddress, wifi_view::Field::kNetmask,
                                                wifi_view::Field::kGateway, wifi_view::Field::kDns};
            wifi_field = fields[(row - 1) & 3];
            open(settings::Page::kWifiStaticText);
        }
    }
    if (static_save) {
        static_save = false;
        const char *wrong = wifi_view::CheckStatic(static_edit);
        if (wrong != nullptr) {
            Toast(wrong);
        } else {
            Job job = MakeJob(JobKind::kWifiStatic);
            snprintf(job.ssid, sizeof job.ssid, "%s", static_ssid);
            job.static_ip = static_edit;
            SendJob(job);
            changed |= nav_state.Back();
        }
    }
    // The keyboard pages: OK and back.
    const bool typed = page == settings::Page::kWifiText || page == settings::Page::kWifiStaticText ||
                       page == settings::Page::kWifiHidden;
    if (!typed) {
        return changed;
    }
    if (password_cancel) {
        password_cancel = false;
        changed |= nav_state.Back();
    }
    if (!password_ready || password_area == nullptr) {
        return changed;
    }
    password_ready = false;
    const char *value = lv_textarea_get_text(password_area);
    const wifi_view::Field f =
        page == settings::Page::kWifiHidden
            ? (hidden_on_password ? wifi_view::Field::kHiddenPassword : wifi_view::Field::kHiddenSsid)
            : wifi_field;
    const char *wrong = wifi_view::Check(f, value);
    if (wrong != nullptr) {
        Toast(wrong);
        return changed;
    }
    if (page == settings::Page::kWifiStaticText) {
        char *field = wifi_view::StaticField(static_edit, f);
        if (field != nullptr) {
            snprintf(field, wifi_view::kIpSize, "%s", value);
        }
        return nav_state.Back() || changed;
    }
    if (page == settings::Page::kWifiHidden && !hidden_on_password) {
        snprintf(hidden_ssid, sizeof hidden_ssid, "%s", value);
        hidden_on_password = true;
        return true;  // the password page
    }
    Job job = MakeJob(page == settings::Page::kWifiHidden ? JobKind::kWifiJoin : JobKind::kWifiText);
    job.which = static_cast<uint8_t>(f);
    snprintf(job.ssid, sizeof job.ssid, "%s", hidden_ssid);
    snprintf(job.password, sizeof job.password, "%s", value);
    SendAndWipe(job);
    if (page == settings::Page::kWifiHidden) {
        hidden_ssid[0] = '\0';
        return nav_state.OpenSettings(static_cast<uint8_t>(settings::Page::kWifi)) || changed;
    }
    return nav_state.Back() || changed;
}

// --- Settings: Display & power -----------------------------------------------------

const DisplayHooks *display_hooks = nullptr;
display_view::View display_shown;
display_view::View display_now;
display_view::Setting choice_setting = display_view::Setting::kIdle;  // the choice page's
int setting_tapped = -1;
int option_tapped = -1;

void OnSettingRow(lv_event_t *event) {
    if (LastWasTap()) {
        setting_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnOption(lv_event_t *event) {
    if (LastWasTap()) {
        option_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}


// The battery, a row per setting, what Power does, "Power off".
void ShowDisplay() {
    TitleBar("Display & power");
    display_hooks->view(&display_shown);
    const display_view::View &v = display_shown;
    int32_t y = kTitleHeight;
    lv_obj_t *bat = Panel(content, y, 56);
    lv_obj_set_style_border_width(bat, 2, 0);
    lv_obj_t *l = Label(bat, &lv_font_montserrat_24);
    lv_label_set_text(l, "Battery");
    lv_obj_set_pos(l, 12, 12);
    char text[48];
    if (v.battery >= 0) {
        snprintf(text, sizeof text, "%d %%%s", v.battery, v.charging ? ", charging" : "");
    } else {
        snprintf(text, sizeof text, "no gauge");
    }
    lv_obj_t *b = Label(bat, &lv_font_montserrat_24);
    lv_label_set_text(b, text);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -12, 0);
    y += 56 + 16;
    constexpr int32_t kRow = 50;  // eight rows
    for (uint8_t i = 0; i < display_view::kSettingCount; ++i, y += kRow) {
        const display_view::Setting s = display_view::kSettings[i];
        display_view::Label(s, v.value[i], text, sizeof text);
        lv_obj_t *row = ChoiceRow(y, display_view::Title(s), text, false, OnSettingRow, i);
        lv_obj_set_height(row, kRow);
    }
    lv_obj_t *help = Label(content, &lv_font_montserrat_20);
    lv_label_set_long_mode(help, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(help, kRowWidth - 24);
    lv_label_set_text(help,
                      "The Power button: twice, the frontlight; three times, off and locked "
                      "(the PIN at the wake); held, off.");
    lv_obj_set_pos(help, 12, y + 20);
    lv_obj_t *off = AskButton(content, kRowWidth - 48, "Power off");
    lv_obj_align(off, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(off, OnAction, LV_EVENT_CLICKED, nullptr);
}

// One setting's choices, a check at the one in use.
void ShowDisplayChoice() {
    TitleBar(display_view::Title(choice_setting));
    size_t n = 0;
    const display_view::Option *o = display_view::Options(choice_setting, &n);
    uint16_t now = 0;
    for (uint8_t i = 0; i < display_view::kSettingCount; ++i) {
        if (display_view::kSettings[i] == choice_setting) {
            now = display_shown.value[i];
        }
    }
    const int in_use = display_view::Index(choice_setting, now);
    int32_t y = kTitleHeight;
    for (size_t i = 0; i < n; ++i, y += kNetRow) {
        ChoiceRow(y, o[i].label, nullptr, static_cast<int>(i) == in_use, OnOption,
                  static_cast<int>(i));
    }
}

// The rows' and choices' taps (Poll). True when the page is to be drawn again.
bool PollDisplay() {
    bool changed = false;
    if (setting_tapped >= 0) {
        if (setting_tapped < display_view::kSettingCount) {
            choice_setting = display_view::kSettings[setting_tapped];
            changed |= nav_state.OpenSettings(static_cast<uint8_t>(settings::Page::kDisplayChoice));
        }
        setting_tapped = -1;
    }
    if (option_tapped >= 0) {
        size_t n = 0;
        const display_view::Option *o = display_view::Options(choice_setting, &n);
        if (static_cast<size_t>(option_tapped) < n) {
            Job job = MakeJob(JobKind::kDisplaySet);
            job.setting = choice_setting;
            job.id = o[option_tapped].value;
            SendJob(job);
            // Shown at once: the save follows in the ui's own task.
            for (uint8_t i = 0; i < display_view::kSettingCount; ++i) {
                if (display_view::kSettings[i] == choice_setting) {
                    display_shown.value[i] = o[option_tapped].value;
                }
            }
            changed |= nav_state.Back();
        }
        option_tapped = -1;
    }
    return changed;
}

// --- Settings: Date & time ---------------------------------------------------------

const TimeHooks *time_hooks = nullptr;
time_view::View time_shown;
time_view::View time_now;
uint8_t zone_region = 0;   // the cities page's region
uint8_t zone_page = 0;     // its page
uint8_t zone_list[96];     // the region's zones (table indices)
uint8_t zone_count = 0;
time_view::Manual manual;  // the hand page's numbers
lv_obj_t *manual_value[time_view::kFields] = {};
int time_row_tapped = -1;  // 0 zone, 1 sync, 2 by hand
int sync_tapped = -1;
int region_tapped = -1;
int city_tapped = -1;
int pager_tapped = 0;   // -1 back, 1 forward
int manual_tapped = -1;  // field * 2 + (1: plus)
bool manual_save = false;

void OnTimeRow(lv_event_t *event) {
    if (LastWasTap()) {
        time_row_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnSyncOption(lv_event_t *event) {
    if (LastWasTap()) {
        sync_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnRegion(lv_event_t *event) {
    if (LastWasTap()) {
        region_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnCity(lv_event_t *event) {
    if (LastWasTap()) {
        city_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnPager(lv_event_t *event) {
    if (LastWasTap()) {
        pager_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

// - / + : no tap check (a quick second press is still a press).
void OnManual(lv_event_t *event) {
    manual_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
}

void OnManualSave(lv_event_t *) {
    if (LastWasTap()) {
        manual_save = true;
    }
}

// A row that only shows: its name, its value.
void InfoRow(int32_t y, const char *name, const char *value) {
    lv_obj_t *row = Panel(content, y, kNetRow);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_t *n = Label(row, &lv_font_montserrat_24);
    lv_label_set_text(n, name);
    lv_obj_set_pos(n, 12, (kNetRow - 30) / 2);
    lv_obj_t *v = Label(row, &lv_font_montserrat_20);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(v, 260, 26);
    lv_label_set_text(v, value);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -12, 0);
}

// The region's zones into zone_list, A to Z by city.
void ListZones(uint8_t region) {
    zone_count = 0;
    const size_t n = time_hooks->zone_count();
    for (size_t i = 0; i < n && zone_count < sizeof zone_list; ++i) {
        if (time_view::RegionOf(time_hooks->zone_name(i)) == region) {
            zone_list[zone_count++] = static_cast<uint8_t>(i);
        }
    }
    for (uint8_t i = 1; i < zone_count; ++i) {  // A to Z by city
        const uint8_t v = zone_list[i];
        uint8_t j = i;
        for (; j > 0 && time_view::CityLess(time_hooks->zone_name(v), time_hooks->zone_name(zone_list[j - 1])); --j) {
            zone_list[j] = zone_list[j - 1];
        }
        zone_list[j] = v;
    }
}

// The clock big, then the zone, the sync, the server, the last sync, by hand.
void ShowTime() {
    TitleBar("Date & time");
    time_hooks->view(&time_shown);
    const time_view::View &v = time_shown;
    lv_obj_t *clock = Label(content, &lv_font_montserrat_48);
    lv_label_set_text(clock, v.time_set ? v.clock : "--:--");
    lv_obj_align(clock, LV_ALIGN_TOP_MID, 0, kTitleHeight + 12);
    lv_obj_t *date = Label(content, &lv_font_montserrat_24);
    lv_label_set_text(date, v.time_set ? v.date : "The clock is not set.");
    lv_obj_align(date, LV_ALIGN_TOP_MID, 0, kTitleHeight + 76);
    lv_obj_t *off = Label(content, &lv_font_montserrat_20);
    lv_label_set_text(off, v.offset);
    lv_obj_align(off, LV_ALIGN_TOP_MID, 0, kTitleHeight + 110);
    int32_t y = kTitleHeight + 150;
    char text[48];
    time_view::City(v.zone, text, sizeof text);
    ChoiceRow(y, "Time zone", text, false, OnTimeRow, 0);
    y += kNetRow;
    time_view::SyncLabel(v.sync_hours, text, sizeof text);
    ChoiceRow(y, "Sync", text, false, OnTimeRow, 1);
    y += kNetRow;
    InfoRow(y, "Server", v.server[0] != '\0' ? v.server : "none");
    y += kNetRow;
    InfoRow(y, "Last sync", v.last_sync);
    y += kNetRow;
    ChoiceRow(y, "Set the time by hand", "", false, OnTimeRow, 2);
    lv_obj_t *b = AskButton(content, kRowWidth - 48, "Sync the time now");
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(b, OnAction, LV_EVENT_CLICKED, nullptr);
}

void ShowTimeSync() {
    TitleBar("Sync");
    const int in_use = time_view::SyncIndex(time_shown.sync_hours);
    int32_t y = kTitleHeight;
    for (uint8_t i = 0; i < time_view::kSyncCount; ++i, y += kNetRow) {
        ChoiceRow(y, time_view::kSync[i].label, nullptr, i == in_use, OnSyncOption, i);
    }
    lv_obj_t *note = Label(content, &lv_font_montserrat_20);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(note, kRowWidth - 24);
    lv_label_set_text(note, "Over Wi-Fi, from the server; the board's clock keeps the time between.");
    lv_obj_set_pos(note, 12, y + 16);
}

void ShowTimeRegion() {
    TitleBar("Time zone");
    const uint8_t in_use = time_view::RegionOf(time_shown.zone);
    int32_t y = kTitleHeight;
    char count[8];
    for (uint8_t r = 0; r < time_view::kRegionCount; ++r, y += kNetRow) {
        ListZones(r);
        snprintf(count, sizeof count, "%u", static_cast<unsigned>(zone_count));
        lv_obj_t *row = ChoiceRow(y, time_view::kRegions[r], count, false, OnRegion, r);
        if (r == in_use) {
            lv_obj_t *mark = Label(row, &lv_font_montserrat_24);
            lv_label_set_text(mark, LV_SYMBOL_OK);
            lv_obj_align(mark, LV_ALIGN_LEFT_MID, 200, 0);
        }
    }
}

// The region's cities, kZoneRows a page; ‹ "1 / 3" › under them.
void ShowTimeZone() {
    TitleBar(time_view::kRegions[zone_region]);
    ListZones(zone_region);
    const uint8_t pages = static_cast<uint8_t>((zone_count + time_view::kZoneRows - 1) / time_view::kZoneRows);
    if (zone_page >= pages) {
        zone_page = 0;
    }
    int32_t y = kTitleHeight;
    char city[48];
    for (uint8_t i = 0; i < time_view::kZoneRows; ++i, y += kNetRow) {
        const unsigned at = zone_page * time_view::kZoneRows + i;
        if (at >= zone_count) {
            break;
        }
        const char *name = time_hooks->zone_name(zone_list[at]);
        time_view::City(name, city, sizeof city);
        ChoiceRow(y, city, nullptr, time_view::Same(name, time_shown.zone), OnCity,
                  static_cast<int>(at));
    }
    if (pages < 2) {
        return;
    }
    lv_obj_t *back = AskButton(content, 120, LV_SYMBOL_LEFT);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_add_event_cb(back, OnPager, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t{-1}));
    lv_obj_t *forward = AskButton(content, 120, LV_SYMBOL_RIGHT);
    lv_obj_align(forward, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_add_event_cb(forward, OnPager, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t{1}));
    char text[12];
    snprintf(text, sizeof text, "%u / %u", zone_page + 1u, static_cast<unsigned>(pages));
    lv_obj_t *p = Label(content, &lv_font_montserrat_24);
    lv_label_set_text(p, text);
    lv_obj_align(p, LV_ALIGN_BOTTOM_MID, 0, -34);
}

void ShowManualValue(uint8_t f) {
    char text[8];
    snprintf(text, sizeof text, f == time_view::kYear ? "%u" : "%02u",
             static_cast<unsigned>(manual.value[f]));
    lv_label_set_text(manual_value[f], text);
}

// Year .. minute, each with - and +; Save sets the clock (local time).
void ShowTimeSet() {
    TitleBar("Set the time");
    constexpr int32_t kRow = 84;
    int32_t y = kTitleHeight + 8;
    for (uint8_t f = 0; f < time_view::kFields; ++f, y += kRow) {
        lv_obj_t *row = Panel(content, y, kRow);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_t *n = Label(row, &lv_font_montserrat_24);
        lv_label_set_text(n, time_view::FieldName(static_cast<time_view::Field>(f)));
        lv_obj_set_pos(n, 12, (kRow - 30) / 2);
        lv_obj_t *minus = AskButton(row, 80, LV_SYMBOL_MINUS);
        lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -232, 0);
        lv_obj_add_event_cb(minus, OnManual, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(f * 2)));
        manual_value[f] = Label(row, &lv_font_montserrat_32);
        lv_obj_set_width(manual_value[f], 110);
        lv_obj_set_style_text_align(manual_value[f], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(manual_value[f], LV_ALIGN_RIGHT_MID, -112, 0);
        ShowManualValue(f);
        lv_obj_t *plus = AskButton(row, 80, LV_SYMBOL_PLUS);
        lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_add_event_cb(plus, OnManual, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(f * 2 + 1)));
    }
    char text[64];
    snprintf(text, sizeof text, "Local time, %s.", time_shown.zone);
    Stub(nullptr, text, y + 8);
    lv_obj_t *b = AskButton(content, kRowWidth - 48, "Save");
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(b, OnManualSave, LV_EVENT_CLICKED, nullptr);
}

// The taps of the Date & time pages (Poll). True when the page is to be drawn again.
bool PollTime() {
    bool changed = false;
    auto open = [&](settings::Page p) {
        changed |= nav_state.OpenSettings(static_cast<uint8_t>(p));
    };
    if (time_row_tapped >= 0) {
        const int row = time_row_tapped;
        time_row_tapped = -1;
        if (row == 0) {
            open(settings::Page::kTimeRegion);
        } else if (row == 1) {
            open(settings::Page::kTimeSync);
        } else {
            for (uint8_t f = 0; f < time_view::kFields; ++f) {
                manual.value[f] = time_shown.now[f];
            }
            if (manual.value[time_view::kYear] < time_view::kFirstYear) {
                manual = {{2026, 1, 1, 12, 0}};  // the clock not set: somewhere to start
            }
            open(settings::Page::kTimeSet);
        }
    }
    if (sync_tapped >= 0) {
        if (sync_tapped < time_view::kSyncCount) {
            Job job = MakeJob(JobKind::kTimeSync);
            job.id = time_view::kSync[sync_tapped].value;
            time_shown.sync_hours = time_view::kSync[sync_tapped].value;
            SendJob(job);
            changed |= nav_state.Back();
        }
        sync_tapped = -1;
    }
    if (region_tapped >= 0) {
        zone_region = static_cast<uint8_t>(region_tapped);
        zone_page = 0;
        region_tapped = -1;
        open(settings::Page::kTimeZone);
    }
    if (pager_tapped != 0) {
        ListZones(zone_region);
        const int pages = (zone_count + time_view::kZoneRows - 1) / time_view::kZoneRows;
        zone_page = static_cast<uint8_t>((zone_page + pages + pager_tapped) % (pages > 0 ? pages : 1));
        pager_tapped = 0;
        changed = true;
    }
    if (city_tapped >= 0) {
        ListZones(zone_region);
        if (city_tapped < zone_count) {
            Job job = MakeJob(JobKind::kTimeZone);
            snprintf(job.password, sizeof job.password, "%s", time_hooks->zone_name(zone_list[city_tapped]));
            snprintf(time_shown.zone, sizeof time_shown.zone, "%.40s", job.password);
            SendJob(job);
            open(settings::Page::kTime);
        }
        city_tapped = -1;
    }
    if (manual_tapped >= 0) {
        const auto f = static_cast<time_view::Field>(manual_tapped / 2);
        time_view::Step(manual, f, manual_tapped % 2 == 1 ? 1 : -1);
        manual_tapped = -1;
        for (uint8_t i = 0; i < time_view::kFields; ++i) {
            if (manual_value[i] != nullptr) {
                ShowManualValue(i);  // the day may move with the month
            }
        }
    }
    if (manual_save) {
        manual_save = false;
        Job job = MakeJob(JobKind::kTimeSet);
        job.manual = manual;
        SendJob(job);
        changed |= nav_state.Back();
    }
    return changed;
}

// --- Settings: Security ------------------------------------------------------------

const SecurityHooks *security_hooks = nullptr;
security_view::View security_shown;
security_view::View security_now;
security_view::PinFlow pin_flow;  // the PIN page's digits (wiped when done)
lv_obj_t *flow_title = nullptr;
lv_obj_t *flow_dots[pinpad::kDigits] = {};
lv_obj_t *flow_message = nullptr;
int security_row_tapped = -1;  // 0 change the PIN, 1 lock, 2 wipe PIN, 3 FIDO2
bool complexity_tapped = false;
int lock_tapped = -1;
bool flow_key = false;
uint32_t flow_key_id = 0;

// The pad's keys (pin_pad_logic.h KeyAt).
const char *const kPinMap[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
                               LV_SYMBOL_CLOSE, "0", LV_SYMBOL_BACKSPACE, ""};

void OnSecurityRow(lv_event_t *event) {
    if (LastWasTap()) {
        security_row_tapped =
            static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnComplexity(lv_event_t *) { complexity_tapped = true; }

void OnLockOption(lv_event_t *event) {
    if (LastWasTap()) {
        lock_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

// A key: handled in Poll (the dots and the title change, a PIN may go out).
void OnFlowKey(lv_event_t *event) {
    flow_key_id = lv_buttonmatrix_get_selected_button(static_cast<lv_obj_t *>(lv_event_get_target(event)));
    flow_key = true;
}

// The vault and FIDO2, Change the PIN, the lock, complexity, the wipe PIN, Lock now.
void ShowSecurity() {
    TitleBar("Security");
    security_hooks->view(&security_shown);
    const security_view::View &v = security_shown;
    char text[64];
    int32_t y = kTitleHeight;
    snprintf(text, sizeof text, "%s, %u records", v.open ? "open" : "locked",
             static_cast<unsigned>(v.records));
    InfoRow(y, "Vault", text);
    y += kNetRow;
    if (v.fido_known) {
        snprintf(text, sizeof text, "%u of %u keys, PIN %s", static_cast<unsigned>(v.fido_keys),
                 static_cast<unsigned>(v.fido_max), v.fido_pin ? "set" : "not set");
    } else {
        snprintf(text, sizeof text, "busy");
    }
    ChoiceRow(y, "FIDO2", text, false, OnSecurityRow, 3);
    y += kNetRow + 16;
    ChoiceRow(y, "Change the PIN", "", false, OnSecurityRow, 0);
    y += kNetRow;
    const int lock = security_view::LockIndex(v.lock_minutes);
    if (lock >= 0) {
        snprintf(text, sizeof text, "%s", security_view::kLock[lock].label);
    } else {
        snprintf(text, sizeof text, "%u min", static_cast<unsigned>(v.lock_minutes));
    }
    char name[40];
    snprintf(name, sizeof name, "%u wrong PINs: lock", static_cast<unsigned>(v.max_wrong));
    ChoiceRow(y, name, text, false, OnSecurityRow, 1);
    y += kNetRow;
    lv_obj_t *row = Panel(content, y, kNetRow);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_t *l = Label(row, &lv_font_montserrat_24);
    lv_label_set_text(l, "PIN complexity");
    lv_obj_set_pos(l, 12, (kNetRow - 30) / 2);
    lv_obj_t *sw = Switch(row, v.complexity);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(sw, OnComplexity, LV_EVENT_VALUE_CHANGED, nullptr);
    y += kNetRow;
    ChoiceRow(y, "Wipe PIN", v.wipe ? "set" : "off", false, OnSecurityRow, 2);
    y += kNetRow;
    lv_obj_t *note = Label(content, &lv_font_montserrat_20);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(note, kRowWidth - 24);
    snprintf(text, sizeof text, "%u wrong PINs in a row now.", static_cast<unsigned>(v.wrong));
    lv_label_set_text(note, v.wrong > 0 ? text
                                        : "Complexity: no repeats, runs or common PINs (FIDO2 "
                                          "from the next start). The wipe PIN, typed on the "
                                          "lock screen, erases the passwords and the FIDO2 keys.");
    lv_obj_set_pos(note, 12, y + 12);
    lv_obj_t *b = AskButton(content, kRowWidth - 48, "Lock now");
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(b, OnAction, LV_EVENT_CLICKED, nullptr);
}

void ShowSecurityLock() {
    TitleBar("Lock after wrong PINs");
    const int in_use = security_view::LockIndex(security_shown.lock_minutes);
    int32_t y = kTitleHeight;
    for (uint8_t i = 0; i < security_view::kLockCount; ++i, y += kNetRow) {
        ChoiceRow(y, security_view::kLock[i].label, nullptr, i == in_use, OnLockOption, i);
    }
    lv_obj_t *note = Label(content, &lv_font_montserrat_20);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(note, kRowWidth - 24);
    lv_label_set_text(note, "How long the PIN is not taken after 10 wrong ones in a row.");
    lv_obj_set_pos(note, 12, y + 16);
}

void ShowFlowDots() {
    for (uint8_t i = 0; i < pinpad::kDigits; ++i) {
        if (flow_dots[i] != nullptr) {
            lv_obj_set_style_bg_opa(flow_dots[i], i < pin_flow.count() ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

// A PIN on a pad of its own: the title says which, six dots, the keys.
void ShowSecurityPin() {
    TitleBar(pin_flow.flow() == security_view::Flow::kChangePin ? "Change the PIN" : "Wipe PIN");
    flow_title = Label(content, &lv_font_montserrat_28);
    lv_label_set_text(flow_title, pin_flow.Title());
    lv_obj_align(flow_title, LV_ALIGN_TOP_MID, 0, kTitleHeight + 20);
    constexpr int32_t kDot = 26;
    constexpr int32_t kGap = 20;
    constexpr int32_t kRowW = pinpad::kDigits * kDot + (pinpad::kDigits - 1) * kGap;
    for (uint8_t i = 0; i < pinpad::kDigits; ++i) {
        lv_obj_t *d = lv_obj_create(content);
        lv_obj_remove_style_all(d);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(d, kDot, kDot);
        lv_obj_set_pos(d, (kRowWidth - kRowW) / 2 + i * (kDot + kGap), kTitleHeight + 80);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 3, 0);
        lv_obj_set_style_border_color(d, lv_color_black(), 0);
        lv_obj_set_style_bg_color(d, lv_color_black(), 0);
        flow_dots[i] = d;
    }
    ShowFlowDots();
    flow_message = Label(content, &lv_font_montserrat_20);
    lv_obj_set_width(flow_message, kRowWidth - 24);
    lv_obj_set_style_text_align(flow_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(flow_message, "");
    lv_obj_align(flow_message, LV_ALIGN_TOP_MID, 0, kTitleHeight + 126);

    lv_obj_t *keys = lv_buttonmatrix_create(content);
    lv_buttonmatrix_set_map(keys, kPinMap);
    lv_obj_set_size(keys, kRowWidth - 40, 440);
    lv_obj_align(keys, LV_ALIGN_BOTTOM_MID, 0, -20);
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
    lv_obj_set_style_outline_width(keys, 0, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(keys, OnFlowKey, LV_EVENT_VALUE_CHANGED, nullptr);
}

void StartFlow(security_view::Flow f) {
    pin_flow.Start(f);
    flow_key = false;
}

// The Security pages' taps (Poll). True when the page is to be drawn again.
bool PollSecurity() {
    bool changed = false;
    const auto page = static_cast<settings::Page>(nav_state.settings_page());
    auto open = [&](settings::Page p) {
        changed |= nav_state.OpenSettings(static_cast<uint8_t>(p));
    };
    if (security_row_tapped >= 0) {
        const int row = security_row_tapped;
        security_row_tapped = -1;
        if (row == 0) {
            StartFlow(security_view::Flow::kChangePin);
            open(settings::Page::kSecurityPin);
        } else if (row == 1) {
            open(settings::Page::kSecurityLock);
        } else if (row == 3) {
            open(settings::Page::kFido);
        } else if (!dialog::IsOpen()) {
            const SheetButton set[] = {{"Set a wipe PIN", 1}, {"Cancel", 3}};
            const SheetButton change[] = {{"Change the wipe PIN", 1}, {"Turn it off", 2}, {"Cancel", 3}};
            if (security_shown.wipe) {
                OpenSheet("Wipe PIN", change, 3);
            } else {
                OpenSheet("Wipe PIN", set, 2);
            }
        }
    }
    if (sheet_choice != 0 && page == settings::Page::kSecurity) {
        const int choice = sheet_choice;
        sheet_choice = 0;
        CloseSheet();
        if (choice == 1) {
            StartFlow(security_view::Flow::kSetWipe);
            open(settings::Page::kSecurityPin);
        } else if (choice == 2) {
            Job job = MakeJob(JobKind::kPinWipe);
            job.flag = false;  // off
            SendJob(job);
            security_shown.wipe = false;
            changed = true;
        }
    }
    if (complexity_tapped) {
        complexity_tapped = false;
        Job job = MakeJob(JobKind::kPinComplexity);
        job.flag = !security_shown.complexity;
        security_shown.complexity = job.flag;
        SendJob(job);
    }
    if (lock_tapped >= 0) {
        if (lock_tapped < security_view::kLockCount) {
            Job job = MakeJob(JobKind::kPinLock);
            job.id = security_view::kLock[lock_tapped].value;
            security_shown.lock_minutes = security_view::kLock[lock_tapped].value;
            SendJob(job);
            changed |= nav_state.Back();
        }
        lock_tapped = -1;
    }
    if (flow_key && page == settings::Page::kSecurityPin) {
        flow_key = false;
        const security_view::Result r = pin_flow.Apply(pinpad::KeyAt(flow_key_id));
        if (r == security_view::Result::kSubmit) {
            Job job = MakeJob(pin_flow.flow() == security_view::Flow::kChangePin ? JobKind::kPinChange
                                                                                 : JobKind::kPinWipe);
            snprintf(job.ssid, sizeof job.ssid, "%s", pin_flow.old());
            snprintf(job.password, sizeof job.password, "%s", pin_flow.fresh());
            job.flag = true;  // kPinWipe: on
            pin_flow.Wipe();
            Toast(job.kind == JobKind::kPinChange ? "changing the PIN..." : "setting the wipe PIN...");
            SendAndWipe(job);
            changed |= nav_state.Back();
        } else if (r != security_view::Result::kNone && flow_title != nullptr) {
            lv_label_set_text(flow_title, pin_flow.Title());
            lv_label_set_text(flow_message, r == security_view::Result::kMismatch
                                                ? "the two differ: the new one again"
                                                : "");
            ShowFlowDots();
        }
    }
    return changed;
}

// --- Settings: About ---------------------------------------------------------------

// The name and the model big, then the lines small; Restart (asked first).
// A snapshot: not looked at again while shown (the uptime and the memory
// would redraw it every time).
void ShowAbout() {
    TitleBar("About");
    char label[32];
    char value[64];
    uint8_t i = 0;
    int32_t y = kTitleHeight + 12;
    if (settings::Line(settings_shown, i, label, sizeof label, value, sizeof value)) {
        lv_obj_t *name = Label(content, &lv_font_montserrat_28);
        lv_label_set_text(name, value);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, y);
        ++i;
    }
    if (settings::Line(settings_shown, i, label, sizeof label, value, sizeof value)) {
        lv_obj_t *model = Label(content, &lv_font_montserrat_20);
        lv_label_set_text(model, value);
        lv_obj_align(model, LV_ALIGN_TOP_MID, 0, y + 40);
        ++i;
    }
    y += 80;
    constexpr int32_t kLine = 40;
    const int32_t bottom = kScreenBottom - kContentTop - 100;
    for (; y + kLine <= bottom && settings::Line(settings_shown, i, label, sizeof label, value,
                                                 sizeof value);
         ++i, y += kLine) {
        lv_obj_t *row = Panel(content, y, kLine);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_t *l = Label(row, &lv_font_montserrat_20);
        lv_label_set_text(l, label);
        lv_obj_set_pos(l, 12, (kLine - 24) / 2);
        lv_obj_t *v = Label(row, &lv_font_montserrat_20);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(v, 290, 24);
        lv_label_set_text(v, value);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    lv_obj_t *b = AskButton(content, kRowWidth - 48, "Restart");
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(b, OnAction, LV_EVENT_CLICKED, nullptr);
}

// --- Settings: Web site ------------------------------------------------------------

const WebHooks *web_hooks = nullptr;
web_view::View web_shown;
web_view::View web_now;
bool login_on_password = false;  // the login page: the user typed, now the password
char login_user[web_view::kUserSize];
int web_row_tapped = -1;  // 0 mode, 2 login
bool web_write_tapped = false;
int web_mode_tapped = -1;

void OnWebRow(lv_event_t *event) {
    if (LastWasTap()) {
        web_row_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

void OnWebWrite(lv_event_t *) { web_write_tapped = true; }

void OnWebMode(lv_event_t *event) {
    if (LastWasTap()) {
        web_mode_tapped = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

// The state, a QR code of the address, when it runs, changes, the login.
void ShowWeb() {
    TitleBar("Web site");
    web_hooks->view(&web_shown);
    const web_view::View &v = web_shown;
    int32_t y = kTitleHeight;
    lv_obj_t *state = Panel(content, y, 52);
    lv_obj_set_style_border_width(state, 2, 0);
    lv_obj_t *s = Label(state, &lv_font_montserrat_24);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s, kRowWidth - 24);
    lv_label_set_text(s, v.state);
    lv_obj_set_pos(s, 12, 10);
    y += 52 + 12;
    if (v.url[0] != '\0') {
        constexpr int32_t kQr = 176;
        lv_obj_t *qr = lv_qrcode_create(content);
        lv_qrcode_set_size(qr, kQr);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_update(qr, v.url, strlen(v.url));
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_t *url = Label(content, &lv_font_montserrat_24);
        lv_label_set_text(url, v.url);
        lv_obj_align(url, LV_ALIGN_TOP_MID, 0, y + kQr + 8);
        y += kQr + 48;
    }
    ChoiceRow(y, "Runs", web_view::ModeLabel(v.mode), false, OnWebRow, 0);
    y += kNetRow;
    lv_obj_t *row = Panel(content, y, kNetRow);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_t *l = Label(row, &lv_font_montserrat_24);
    lv_label_set_text(l, "Changes from the site");
    lv_obj_set_pos(l, 12, (kNetRow - 30) / 2);
    lv_obj_t *sw = Switch(row, v.write);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(sw, OnWebWrite, LV_EVENT_VALUE_CHANGED, nullptr);
    y += kNetRow;
    ChoiceRow(y, "Login", v.login ? v.user : "off", false, OnWebRow, 2);
    y += kNetRow;
    lv_obj_t *note = Label(content, &lv_font_montserrat_20);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(note, kRowWidth - 24);
    lv_label_set_text(note, v.login ? "The browser asks for the login."
                                    : "No login: anyone on the network may open the site.");
    lv_obj_set_pos(note, 12, y + 12);
}

void ShowWebMode() {
    TitleBar("Web site runs");
    int32_t y = kTitleHeight;
    for (uint8_t i = 0; i < web_view::kModeCount; ++i, y += kNetRow) {
        ChoiceRow(y, web_view::kModes[i].label, nullptr, web_view::kModes[i].value == web_shown.mode,
                  OnWebMode, i);
    }
    lv_obj_t *note = Label(content, &lv_font_montserrat_20);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(note, kRowWidth - 24);
    lv_label_set_text(note, "The access point: the one the device opens when no network "
                            "answers (Wi-Fi), or always in its AP mode.");
    lv_obj_set_pos(note, 12, y + 16);
}

// The user, then the password (hidden), on the keyboard; OK goes on.
void ShowWebLogin() {
    if (login_on_password) {
        KeyboardPage("Login: password", "password", "", true, web_view::kPasswordSize - 1,
                     "1..64 characters. " LV_SYMBOL_OK ": save, " LV_SYMBOL_KEYBOARD ": back.");
    } else {
        KeyboardPage("Login: user", "user", "", false, web_view::kUserSize - 1,
                     "1..32 characters, no ':'. " LV_SYMBOL_OK ": the password next.");
    }
}

// The Web site pages' taps (Poll). True when the page is to be drawn again.
bool PollWeb() {
    bool changed = false;
    const auto page = static_cast<settings::Page>(nav_state.settings_page());
    auto open = [&](settings::Page p) {
        changed |= nav_state.OpenSettings(static_cast<uint8_t>(p));
    };
    if (web_row_tapped >= 0) {
        const int row = web_row_tapped;
        web_row_tapped = -1;
        if (row == 0) {
            open(settings::Page::kWebMode);
        } else if (!dialog::IsOpen()) {
            const SheetButton set[] = {{"Set a login", 1}, {"Cancel", 3}};
            const SheetButton change[] = {{"Change the login", 1}, {"Turn the login off", 2}, {"Cancel", 3}};
            if (web_shown.login) {
                OpenSheet("Login", change, 3);
            } else {
                OpenSheet("Login", set, 2);
            }
        }
    }
    if (sheet_choice != 0 && page == settings::Page::kWeb) {
        const int choice = sheet_choice;
        sheet_choice = 0;
        CloseSheet();
        if (choice == 1) {
            login_on_password = false;
            open(settings::Page::kWebLogin);
        } else if (choice == 2) {
            Job job = MakeJob(JobKind::kWebLogin);
            job.flag = false;  // off
            SendJob(job);
            web_shown.login = false;
            changed = true;
        }
    }
    if (web_write_tapped) {
        web_write_tapped = false;
        Job job = MakeJob(JobKind::kWebWrite);
        job.flag = !web_shown.write;
        web_shown.write = job.flag;
        SendJob(job);
    }
    if (web_mode_tapped >= 0) {
        if (web_mode_tapped < web_view::kModeCount) {
            Job job = MakeJob(JobKind::kWebMode);
            job.id = web_view::kModes[web_mode_tapped].value;
            web_shown.mode = web_view::kModes[web_mode_tapped].value;
            SendJob(job);
            changed |= nav_state.Back();
        }
        web_mode_tapped = -1;
    }
    if (page != settings::Page::kWebLogin) {
        return changed;
    }
    if (password_cancel) {
        password_cancel = false;
        vault::Wipe(login_user, sizeof login_user);
        changed |= nav_state.Back();
    }
    if (password_ready && password_area != nullptr) {
        password_ready = false;
        const char *typed = lv_textarea_get_text(password_area);
        if (!login_on_password) {
            if (!web_view::UserOk(typed)) {
                Toast("1..32 characters, no ':'");
            } else {
                snprintf(login_user, sizeof login_user, "%s", typed);
                login_on_password = true;
                changed = true;  // the password page
            }
        } else if (!web_view::PasswordOk(typed)) {
            Toast("1..64 characters");
        } else {
            Job job = MakeJob(JobKind::kWebLogin);
            job.flag = true;
            snprintf(job.ssid, sizeof job.ssid, "%s", login_user);
            snprintf(job.password, sizeof job.password, "%s", typed);
            vault::Wipe(login_user, sizeof login_user);
            SendAndWipe(job);
            changed |= nav_state.Back();
        }
    }
    return changed;
}

// --- Settings: FIDO2 / U2F ---------------------------------------------------------

bool show_keys_tapped = false;

void OnShowKeys(lv_event_t *) {
    if (LastWasTap()) {
        show_keys_tapped = true;
    }
}

// The state first (big), then the lines small; Show the keys, Reset FIDO2
// (held). A snapshot, as About: drawn again only when opened.
void ShowFidoSettings() {
    TitleBar("FIDO2 / U2F");
    char label[32];
    char value[64];
    uint8_t i = 0;
    int32_t y = kTitleHeight + 8;
    if (settings::Line(settings_shown, i, label, sizeof label, value, sizeof value)) {
        lv_obj_t *state = Label(content, &lv_font_montserrat_24);
        lv_label_set_long_mode(state, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(state, kRowWidth - 24);
        lv_label_set_text(state, value);
        lv_obj_set_pos(state, 12, y);
        ++i;
    }
    y += 44;
    constexpr int32_t kLine = 40;
    const int32_t bottom = kScreenBottom - kContentTop - 190;
    for (; y + kLine <= bottom && settings::Line(settings_shown, i, label, sizeof label, value,
                                                 sizeof value);
         ++i, y += kLine) {
        lv_obj_t *row = Panel(content, y, kLine);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_t *l = Label(row, &lv_font_montserrat_20);
        lv_label_set_text(l, label);
        lv_obj_set_pos(l, 12, (kLine - 24) / 2);
        lv_obj_t *v = Label(row, &lv_font_montserrat_20);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(v, 270, 24);
        lv_label_set_text(v, value);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    lv_obj_t *keys = AskButton(content, kRowWidth - 48, "Show the keys");
    lv_obj_align(keys, LV_ALIGN_BOTTOM_MID, 0, -104);
    lv_obj_add_event_cb(keys, OnShowKeys, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *reset = AskButton(content, kRowWidth - 48, "Reset FIDO2");
    lv_obj_align(reset, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(reset, OnAction, LV_EVENT_CLICKED, nullptr);
}

// Show the keys: the U2F / FIDO2 list. Reset: the held question (Poll
// sends the reset on its Yes). True when the page is to be drawn again.
bool PollFidoSettings() {
    bool changed = false;
    if (show_keys_tapped) {
        show_keys_tapped = false;
        changed |= nav_state.Open(nav::Mode::kFido);
    }
    if (fido_reset_tapped) {
        fido_reset_tapped = false;
        if (!dialog::IsOpen()) {
            dialog::Confirm("Reset FIDO2?", nullptr, nullptr,
                            "Every key and the FIDO2 PIN are erased: the sites will not take "
                            "this device any more. Sign in another way first.",
                            "Reset: hold 4 s", kResetNo, kResetYes);
        }
    }
    return changed;
}

// --- Settings: the touch's calibration ---------------------------------------------

bool cal_active = false;  // the crosses are up: no calibration in use
touch_cal::Calibration cal_before;
uint8_t cal_step = 0;  // the cross to tap; kTargets: the result
ScreenPoint cal_taps[touch_cal::kTargets];
touch_cal::Fit cal_fit = {};
bool cal_tapped = false;
int cal_button = 0;  // 1 save, 2 again, 3 default

void OnCalPad(lv_event_t *) {
    if (LastWasTap()) {
        cal_tapped = true;
    }
}

void OnCalButton(lv_event_t *event) {
    if (LastWasTap()) {
        cal_button = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
}

// A cross at screen point p.
void AddCross(ScreenPoint p) {
    const int32_t x = p.x - kLeft;
    const int32_t y = p.y - kContentTop;
    lv_obj_t *h = lv_obj_create(content);
    lv_obj_remove_style_all(h);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(h, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_size(h, 50, 3);
    lv_obj_set_pos(h, x - 25, y - 1);
    lv_obj_t *v = lv_obj_create(content);
    lv_obj_remove_style_all(v);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(v, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_size(v, 3, 50);
    lv_obj_set_pos(v, x - 1, y - 25);
    lv_obj_t *ring = lv_obj_create(content);
    lv_obj_remove_style_all(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_black(), 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(ring, 24, 24);
    lv_obj_set_pos(ring, x - 12, y - 12);
}

void StartCalibration() {
    if (!cal_active) {
        cal_before = TouchCalibration();
        cal_active = true;
    }
    UseTouchCalibration(touch_cal::Calibration{});  // the crosses take raw taps
    cal_step = 0;
}

// Leaving the page without a save: the old one back.
void EndCalibration() {
    if (cal_active) {
        UseTouchCalibration(cal_before);
        cal_active = false;
    }
}

// One cross at a time, then the fit and Save / Again / Default.
void ShowTouch() {
    TitleBar("Touchscreen");
    char text[160];
    if (cal_step < touch_cal::kTargets) {
        // Under the crosses, over the page: every tap counts.
        lv_obj_t *pad = lv_obj_create(content);
        lv_obj_remove_style_all(pad);
        lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(pad, 0, kTitleHeight);
        lv_obj_set_size(pad, kRowWidth, kScreenBottom - kContentTop - kTitleHeight);
        lv_obj_add_event_cb(pad, OnCalPad, LV_EVENT_CLICKED, nullptr);
        snprintf(text, sizeof text, "Tap the middle of the cross (%u of %u).",
                 static_cast<unsigned>(cal_step + 1), static_cast<unsigned>(touch_cal::kTargets));
        Stub(nullptr, text, 250);
        AddCross(touch_cal::kTarget[cal_step]);
        return;
    }
    const touch_cal::Calibration &c = cal_fit.calibration;
    if (cal_fit.ok) {
        snprintf(text, sizeof text,
                 "The taps fit.\n\nAcross: x %d.%03d, %+d px\nDown: x %d.%03d, %+d px\nThe "
                 "farthest tap: %d px off.",
                 c.x_scale / 1000, c.x_scale % 1000, c.x_offset, c.y_scale / 1000,
                 c.y_scale % 1000, c.y_offset, static_cast<int>(cal_fit.worst));
    } else {
        snprintf(text, sizeof text,
                 "The taps do not fit (one beside its cross?): try again, or keep the default.");
    }
    Stub(nullptr, text, kTitleHeight + 24);
    const SheetButton buttons[] = {{"Save", 1}, {"Again", 2}, {"Use the default", 3}};
    int32_t y = -16;
    for (int i = 2; i >= (cal_fit.ok ? 0 : 1); --i, y -= 88) {
        lv_obj_t *b = AskButton(content, kRowWidth - 48, buttons[i].label);
        lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, y);
        lv_obj_add_event_cb(b, OnCalButton, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(buttons[i].id)));
    }
}

// The crosses' taps and the result's buttons (Poll). True when the page is to
// be drawn again.
bool PollTouch() {
    if (nav_state.settings_page() != static_cast<uint8_t>(settings::Page::kTouch)) {
        return false;
    }
    bool changed = false;
    if (cal_tapped) {
        cal_tapped = false;
        if (cal_step < touch_cal::kTargets) {
            cal_taps[cal_step++] = RawDown();
            if (cal_step == touch_cal::kTargets) {
                cal_fit = touch_cal::Solve(cal_taps);
            }
            changed = true;
        }
    }
    if (cal_button != 0) {
        const int b = cal_button;
        cal_button = 0;
        if (b == 2) {
            StartCalibration();
            return true;
        }
        Job job = MakeJob(JobKind::kSaveTouch);
        job.calibration = b == 1 && cal_fit.ok ? cal_fit.calibration : touch_cal::Calibration{};
        cal_before = job.calibration;  // in use from now
        EndCalibration();
        SendJob(job);
        changed |= nav_state.Back();
    }
    return changed;
}

void ShowSettings() {
    const auto page = static_cast<settings::Page>(nav_state.settings_page());
    if (page == settings::Page::kTouch) {
        if (!cal_active) {
            StartCalibration();
        }
        ShowTouch();
        return;
    }
    EndCalibration();
    if (page != settings::Page::kBlePair) {
        ClosePairing();
    }
    if (ble_hooks != nullptr && page == settings::Page::kBluetooth) {
        ShowBluetooth();
        return;
    }
    if (page != settings::Page::kSecurityPin) {
        pin_flow.Wipe();
    }
    if (page == settings::Page::kAbout && settings_hooks != nullptr) {
        SettingsText(page, settings_shown, sizeof settings_shown);
        ShowAbout();
        return;
    }
    if (page != settings::Page::kWebLogin) {
        login_on_password = page == settings::Page::kWebLogin && login_on_password;
    }
    if (page == settings::Page::kFido && settings_hooks != nullptr) {
        SettingsText(page, settings_shown, sizeof settings_shown);
        ShowFidoSettings();
        return;
    }
    if (web_hooks != nullptr) {
        switch (page) {
            case settings::Page::kWeb: ShowWeb(); return;
            case settings::Page::kWebMode: ShowWebMode(); return;
            case settings::Page::kWebLogin: ShowWebLogin(); return;
            default: break;
        }
    }
    if (security_hooks != nullptr) {
        switch (page) {
            case settings::Page::kSecurity: ShowSecurity(); return;
            case settings::Page::kSecurityLock: ShowSecurityLock(); return;
            case settings::Page::kSecurityPin: ShowSecurityPin(); return;
            default: break;
        }
    }
    if (time_hooks != nullptr) {
        switch (page) {
            case settings::Page::kTime: ShowTime(); return;
            case settings::Page::kTimeSync: ShowTimeSync(); return;
            case settings::Page::kTimeRegion: ShowTimeRegion(); return;
            case settings::Page::kTimeZone: ShowTimeZone(); return;
            case settings::Page::kTimeSet: ShowTimeSet(); return;
            default: break;
        }
    }
    if (display_hooks != nullptr && page == settings::Page::kDisplay) {
        ShowDisplay();
        return;
    }
    if (display_hooks != nullptr && page == settings::Page::kDisplayChoice) {
        ShowDisplayChoice();
        return;
    }
    if (ble_hooks != nullptr && page == settings::Page::kBlePair) {
        ShowBlePair();
        return;
    }
    if (ble_hooks != nullptr && page == settings::Page::kBleName) {
        KeyboardPage("Bluetooth name", "name", ble_shown.name, false, ble_view::kNameSize - 1,
                     "What hosts see when they look for devices: 1..29 characters. "
                     LV_SYMBOL_OK ": save, " LV_SYMBOL_KEYBOARD ": back.");
        return;
    }
    if (wifi_hooks != nullptr && page == settings::Page::kWifi) {
        ShowWifi();
        return;
    }
    if (wifi_hooks != nullptr && page == settings::Page::kWifiScan) {
        ShowWifiScan();
        return;
    }
    if (wifi_hooks != nullptr && page == settings::Page::kWifiPassword) {
        ShowWifiPassword();
        return;
    }
    if (wifi_hooks != nullptr) {
        switch (page) {
            case settings::Page::kWifiMore: ShowWifiMore(); return;
            case settings::Page::kWifiChoice: ShowWifiChoice(); return;
            case settings::Page::kWifiText:
                ShowWifiText(wifi_field, wifi_field == wifi_view::Field::kApName    ? wifi_shown.ap_name
                                         : wifi_field == wifi_view::Field::kHostname ? wifi_shown.hostname
                                                                                    : "");
                return;
            case settings::Page::kWifiStatic: ShowWifiStatic(); return;
            case settings::Page::kWifiStaticText:
                ShowWifiText(wifi_field, wifi_view::StaticField(static_edit, wifi_field));
                return;
            case settings::Page::kWifiHidden:
                ShowWifiText(hidden_on_password ? wifi_view::Field::kHiddenPassword
                                                : wifi_view::Field::kHiddenSsid,
                             "");
                return;
            default: break;
        }
    }
    if (settings_hooks == nullptr) {
        TitleBar("Settings");
        Stub(nullptr, "Not on this device.", kTitleHeight + 24);
        return;
    }
    SettingsText(page, settings_shown, sizeof settings_shown);
    settings_looked_at = lv_tick_get();
    if (page == settings::Page::kMain) {
        ShowSettingsMain();
    } else {
        ShowSettingsPage(page);
    }
}

// The FIDO2 list's pages: back, "1 / 2" (the current one, inverted), forward.
void ShowFidoBar() {
    snprintf(tab_text[1], sizeof tab_text[1], "%u / %u", nav_state.fido_page() + 1u,
             static_cast<unsigned>(nav_state.fido_pages()));
    tab_map[0] = LV_SYMBOL_LEFT;
    tab_map[1] = tab_text[1];
    tab_map[2] = LV_SYMBOL_RIGHT;
    tab_map[nav::Nav::kFidoBar] = "";
    lv_buttonmatrix_set_map(tabs, tab_map);
    lv_buttonmatrix_clear_button_ctrl_all(tabs, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_buttonmatrix_set_button_ctrl(tabs, 1, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_HIDDEN);
}

// A tab per page, at most nav::kTabs; arrows where more pages are hidden.
void ShowTabs() {
    const uint8_t n = nav_state.tabs();
    if (nav_state.mode() == nav::Mode::kFido && nav_state.fido_pages() > 1) {
        ShowFidoBar();
        return;
    }
    if (nav_state.mode() != nav::Mode::kList || nav_state.pages() < 2) {
        lv_obj_add_flag(tabs, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    for (uint8_t i = 0; i < n; ++i) {
        const unsigned p = nav_state.first() + i;
        const bool more_left = i == 0 && nav_state.first() > 0;
        const bool more_right = i + 1 == n && p + 1 < nav_state.pages();
        snprintf(tab_text[i], sizeof tab_text[i], "%s%u%s", more_left ? LV_SYMBOL_LEFT " " : "",
                 p + 1, more_right ? " " LV_SYMBOL_RIGHT : "");
        tab_map[i] = tab_text[i];
    }
    tab_map[n] = "";
    lv_buttonmatrix_set_map(tabs, tab_map);
    lv_buttonmatrix_clear_button_ctrl_all(tabs, LV_BUTTONMATRIX_CTRL_CHECKED);
    for (uint8_t i = 0; i < n; ++i) {
        if (nav_state.first() + i == nav_state.page()) {
            lv_buttonmatrix_set_button_ctrl(tabs, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
    }
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_HIDDEN);
}

void ShowList() {
    seen = Look();
    nav_state.SetPages(nav::PagesFor(seen.count, list::kRowsPerPage));
    if (seen.state != vault::State::kOpen) {
        Stub("Passwords", "The vault is locked.");
        return;
    }
    if (seen.count == 0) {
        Stub("Passwords",
             "No records yet: add them on the web site (Passwords) or on the console "
             "(pass add).");
        return;
    }
    AddHints();
    const list::Span span = list::PageSpan(seen.count, nav_state.page());
    for (uint8_t i = 0; i < span.count; ++i) {
        if (vault::Get(span.first + i, &record) != vault::Error::kOk) {
            break;
        }
        AddRow(i, record);
        ++row_count;
    }
    vault::Wipe(&record, sizeof record);
    UpdateCodes(time(nullptr));
}

// The current mode and page, drawn anew.
void Show() {
    for (uint8_t i = 0; i < row_count; ++i) {
        if (rows[i].shown) {
            SetWiped(rows[i].pass, "");
        }
    }
    row_count = 0;
    fido_rows = 0;
    // A redraw takes the can and any dialog away (its question unanswered).
    remove_state.Clear();
    light_switch = nullptr;  // deleted with the page
    memset(manual_value, 0, sizeof manual_value);
    memset(flow_dots, 0, sizeof flow_dots);
    flow_title = nullptr;
    flow_message = nullptr;
    if (nav_state.mode() != nav::Mode::kSettings) {
        pin_flow.Wipe();
    }
    WipePassword();
    CloseSheet();
    if (nav_state.mode() != nav::Mode::kSettings) {
        EndCalibration();
        ClosePairing();
    }
    lv_obj_clean(content);
    lv_obj_set_height(content, (nav_state.mode() == nav::Mode::kSettings ? kScreenBottom
                                                                           : kContentBottom) -
                                   kContentTop);
    switch (nav_state.mode()) {
        case nav::Mode::kList:
            ShowList();
            break;
        case nav::Mode::kSettings:
            ShowSettings();
            break;
        case nav::Mode::kFido:
            ShowFido();
            break;
    }
    ShowTabs();
}

void OnTab(lv_event_t *event) {
    auto *m = static_cast<lv_obj_t *>(lv_event_get_target(event));
    tapped_tab = static_cast<int>(lv_buttonmatrix_get_selected_button(m));
}

void StartTabs(lv_obj_t *scr) {
    tabs = lv_buttonmatrix_create(scr);
    tab_map[0] = "";
    lv_buttonmatrix_set_map(tabs, tab_map);
    lv_obj_set_pos(tabs, kLeft, kContentBottom);
    lv_obj_set_size(tabs, kRowWidth, kTabHeight);
    lv_obj_set_style_bg_color(tabs, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(tabs, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(tabs, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(tabs, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(tabs, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tabs, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(tabs, 6, LV_PART_MAIN);
    // E-ink: no pressed look (as the PIN pad); the current page inverted.
    const lv_style_selector_t kStates[] = {LV_STATE_DEFAULT, LV_STATE_PRESSED, LV_STATE_FOCUSED,
                                           LV_STATE_FOCUS_KEY};
    for (const lv_style_selector_t state : kStates) {
        const auto part = static_cast<lv_style_selector_t>(LV_PART_ITEMS) | state;
        lv_obj_set_style_bg_color(tabs, lv_color_white(), part);
        lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, part);
        lv_obj_set_style_border_width(tabs, 2, part);
        lv_obj_set_style_border_color(tabs, lv_color_black(), part);
        lv_obj_set_style_outline_width(tabs, 0, part);
        lv_obj_set_style_shadow_width(tabs, 0, part);
        lv_obj_set_style_radius(tabs, 10, part);
        lv_obj_set_style_text_color(tabs, lv_color_black(), part);
        lv_obj_set_style_text_font(tabs, &lv_font_montserrat_24, part);
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

// --- typing ------------------------------------------------------------------------

const char *TypeJob(const Job &job) {
    if (!Fetch(job.id, &type_record)) {
        return "the record is gone";
    }
    char code[8] = "";
    const char *text = job.zone == list::Zone::kTypeUser ? type_record.user
                       : job.zone == list::Zone::kTypePassword ? type_record.password
                                                               : code;
    if (job.zone == list::Zone::kTypeTotp) {
        const time_t now = time(nullptr);
        totp::Codes codes = {};
        if (!TimeSet(now) || totp::Generate(type_record.otp, now, &codes) != ESP_OK) {
            return "no TOTP code: the clock is not set";
        }
        snprintf(code, sizeof code, "%06u", static_cast<unsigned>(codes.code[1] % 1000000));
    }
    if (text[0] == '\0') {
        return job.zone == list::Zone::kTypeUser ? "no user" : "no password";
    }
    const esp_err_t err = typist != nullptr ? typist(text) : ESP_ERR_NOT_SUPPORTED;
    vault::Wipe(code, sizeof code);
    switch (err) {
        case ESP_OK:
            return job.zone == list::Zone::kTypeUser       ? "typed the user"
                   : job.zone == list::Zone::kTypePassword ? "typed the password"
                                                           : "typed the TOTP code";
        case ESP_ERR_INVALID_STATE:
            return "no keyboard host connected";
        case ESP_ERR_INVALID_ARG:
            return "cannot type it: a character not on a US keyboard";
        default:
            return "cannot type";
    }
}

// A key from the U2F / FIDO2 list (a made-up one only from the screen).
const char *DeleteJob(const Job &job) {
    if (memcmp(job.key, kDemoId, sizeof kDemoId) == 0) {
        Lock();
        demo_gone |= 1u << (job.key[sizeof kDemoId] % 32);
        Unlock();
        return "made-up key deleted";
    }
    switch (fido_ctap_delete(job.key)) {
        case ESP_OK:
            return "key deleted";
        case ESP_ERR_NOT_FOUND:
            return "the key is gone already";
        case ESP_ERR_TIMEOUT:
            return "busy: a FIDO2 request is running";
        default:
            return "cannot delete the key";
    }
}

void TypeTask(void *) {
    for (;;) {
        Job job = {};
        xQueueReceive(jobs, &job, portMAX_DELAY);
        if (job.kind == JobKind::kDeleteKey) {
            const char *said = DeleteJob(job);
            Lock();
            fido_seen.err = ESP_FAIL;  // the list read again at the next look
            fido_looked_at = lv_tick_get() - kFidoMs;
            Toast(said);
            Unlock();
            continue;
        }
        if (job.kind == JobKind::kSaveLight) {
            const esp_err_t err = settings_hooks->save_light();
            if (err != ESP_OK) {
                Lock();
                Toast("the light level is not saved");
                Unlock();
            }
            continue;
        }
        if (job.kind == JobKind::kAct) {
            const char *said = settings_hooks->act(job.action);
            Lock();
            settings_looked_at = lv_tick_get() - kSettingsMs;  // shown again soon
            Toast(said);
            Unlock();
            continue;
        }
        if (job.kind == JobKind::kWifiScan) {
            WifiScanJob();
            continue;
        }
        if (job.kind >= JobKind::kWifiActive) {
            const char *said = "";
            switch (job.kind) {
                case JobKind::kWifiActive: said = wifi_hooks->set_active(job.flag); break;
                case JobKind::kWifiJoin:
                    said = wifi_hooks->join(job.ssid, job.flag ? nullptr : job.password);
                    break;
                case JobKind::kWifiForget: said = wifi_hooks->forget(job.ssid); break;
                case JobKind::kBleActive: said = ble_hooks->set_active(job.flag); break;
                case JobKind::kBleUse: said = ble_hooks->use(job.flag ? nullptr : job.ssid); break;
                case JobKind::kBleForget: said = ble_hooks->forget(job.ssid); break;
                case JobKind::kBlePair: said = ble_hooks->pair(job.flag); break;
                case JobKind::kBleName: said = ble_hooks->set_name(job.flag ? job.ssid : nullptr); break;
                case JobKind::kTimeZone: said = time_hooks->set_zone(job.password); break;
                case JobKind::kTimeSync:
                    said = time_hooks->set_sync(static_cast<uint8_t>(job.id));
                    break;
                case JobKind::kTimeSet: said = time_hooks->set_time(job.manual); break;
                case JobKind::kPinChange:
                    said = security_hooks->change_pin(job.ssid, job.password);
                    break;
                case JobKind::kPinWipe:
                    said = security_hooks->set_wipe(job.flag ? job.password : nullptr);
                    break;
                case JobKind::kPinLock:
                    said = security_hooks->set_lock(static_cast<uint16_t>(job.id));
                    break;
                case JobKind::kPinComplexity: said = security_hooks->set_complexity(job.flag); break;
                case JobKind::kWebMode: said = web_hooks->set_mode(static_cast<uint8_t>(job.id)); break;
                case JobKind::kWifiChoice:
                    said = wifi_hooks->set_choice(static_cast<wifi_view::Choice>(job.which),
                                                  static_cast<uint16_t>(job.id));
                    break;
                case JobKind::kWifiText:
                    said = wifi_hooks->set_text(static_cast<wifi_view::Field>(job.which), job.password);
                    break;
                case JobKind::kWifiStatic: said = wifi_hooks->set_static(job.ssid, job.static_ip); break;
                case JobKind::kWebWrite: said = web_hooks->set_write(job.flag); break;
                case JobKind::kWebLogin:
                    said = web_hooks->set_login(job.flag ? job.ssid : nullptr,
                                                job.flag ? job.password : nullptr);
                    break;
                case JobKind::kDisplaySet:
                    said = display_hooks->set(job.setting, static_cast<uint16_t>(job.id));
                    break;
                case JobKind::kSaveTouch:
                    said = SaveTouchCalibration(job.calibration) == ESP_OK
                               ? "saved"
                               : "in use, but config.json is not saved";
                    break;
                default: break;
            }
            vault::Wipe(&job, sizeof job);
            Lock();
            settings_looked_at = lv_tick_get() - kSettingsMs;
            Toast(said);
            Unlock();
            continue;
        }
        const char *said = TypeJob(job);
        vault::Wipe(&type_record, sizeof type_record);
        Lock();
        Toast(said);
        Unlock();
    }
}

// --- firmware update ----------------------------------------------------------------

constexpr int32_t kUpdateBarHeight = 48;
constexpr uint32_t kUpdateFailedMs = 8000;

lv_obj_t *update_box = nullptr;  // under Lock
lv_obj_t *update_title = nullptr;
lv_obj_t *update_line = nullptr;
lv_obj_t *update_fill = nullptr;
lv_obj_t *update_percent = nullptr;
lv_obj_t *update_note = nullptr;
int update_shown = -1;  // the percent on the glass, -1 none

lv_obj_t *UpdateLabel(const lv_font_t *font, int32_t y) {
    lv_obj_t *l = Label(update_box, font);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, kRowWidth - 40);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// Over everything (the top layer, under the status bar), as FIDO2's prompt.
void OpenUpdate() {
    update_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(update_box);
    lv_obj_add_flag(update_box, LV_OBJ_FLAG_CLICKABLE);  // the page under it takes no taps
    lv_obj_remove_flag(update_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(update_box, kLeft, kContentTop);
    lv_obj_set_size(update_box, kRowWidth, kScreenBottom - kContentTop);
    lv_obj_set_style_bg_color(update_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(update_box, LV_OPA_COVER, 0);
    lv_obj_move_to_index(update_box, 0);  // under the status bar

    update_title = UpdateLabel(&lv_font_montserrat_32, 60);
    lv_label_set_text(update_title, "Firmware update");
    update_line = UpdateLabel(&lv_font_montserrat_24, 140);

    lv_obj_t *frame = lv_obj_create(update_box);
    lv_obj_remove_style_all(frame);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(frame, kRowWidth - 40, kUpdateBarHeight);
    lv_obj_set_style_border_width(frame, 3, 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_set_style_radius(frame, 8, 0);
    lv_obj_set_style_pad_all(frame, 6, 0);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 240);
    update_fill = lv_obj_create(frame);
    lv_obj_remove_style_all(update_fill);
    lv_obj_set_style_bg_color(update_fill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(update_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(update_fill, 4, 0);
    lv_obj_set_size(update_fill, 0, LV_PCT(100));

    update_percent = UpdateLabel(&lv_font_montserrat_28, 240 + kUpdateBarHeight + 20);
    update_note = UpdateLabel(&lv_font_montserrat_20, 420);
    lv_label_set_text(update_note, "Do not turn the device off. If the new firmware does not "
                                   "start, the next restart brings this one back.");
    update_shown = -1;
}

void CloseUpdate(lv_timer_t *) {
    if (update_box != nullptr) {
        lv_obj_delete(update_box);
        update_box = nullptr;
    }
    update_shown = -1;
}

void DrawUpdatePercent(uint8_t percent) {
    const int32_t inner = kRowWidth - 40 - 2 * 3 - 2 * 6;
    lv_obj_set_width(update_fill, inner * percent / 100);
    lv_label_set_text_fmt(update_percent, "%u %%", static_cast<unsigned>(percent));
    update_shown = percent;
}

// --- the ui task's timers ------------------------------------------------------------

// Buttons, swipes, the vault's changes, the codes, the hidden passwords.
void Poll(lv_timer_t *) {
    uint8_t in = 0;
    bool changed = false;
    while (xQueueReceive(inputs, &in, 0) == pdTRUE) {
        const auto input = static_cast<nav::Input>(in);
        if (update_box != nullptr) {  // a firmware update: nothing to choose
            continue;
        }
        if (!HomeShown()) {  // under the PIN pad the buttons do nothing here
            continue;
        }
        if (dialog::IsOpen() && input != nav::Input::kHome) {
            if (input == nav::Input::kRight) {
                dialog::Cancel();  // Right: No (as FIDO2's prompt), or Cancel
            }
            continue;
        }
        changed |= nav_state.Apply(input);
    }
    if (open_mode >= 0) {
        changed |= nav_state.Open(static_cast<nav::Mode>(open_mode));
        open_mode = -1;
    }
    if (back_tapped) {
        changed |= nav_state.Back();
        back_tapped = false;
    }
    if (tapped_tab >= 0) {
        changed |= nav_state.Tab(static_cast<uint8_t>(tapped_tab));
        tapped_tab = -1;
    }
    const nav::Gesture g = TakeGesture();
    if (g == nav::Gesture::kLeft || g == nav::Gesture::kRight) {
        if (nav_state.mode() == nav::Mode::kFido && HomeShown() && !dialog::IsOpen()) {
            const uint8_t row = fido_list::RowAt(GestureStart().y - kContentTop, kTitleHeight,
                                                 kFidoRowHeight, fido_rows);
            if (remove_state.Swipe(g, row)) {
                ShowTrash();
            }
        }
    } else if (g != nav::Gesture::kNone && HomeShown() && !dialog::IsOpen()) {
        changed |= nav_state.Swipe(g);
    }
    if (row_tapped) {
        row_tapped = false;
        if (remove_state.Drop()) {
            ShowTrash();
        }
    }
    if (trash_tapped) {
        trash_tapped = false;
        if (!dialog::IsOpen()) {
            AskRemove();
        }
    }
    // A dialog's answer: the held questions here, the choices to their pages.
    switch (const int answer = dialog::Poll()) {
        case dialog::kNone:
            break;
        case kDeleteYes:
            DeleteAsked();
            break;
        case kDeleteNo:
            remove_state.Clear();
            ShowTrash();
            break;
        case kResetYes: {
            Job job = MakeJob(JobKind::kAct);
            job.action = settings::Action::kFidoReset;
            Toast("resetting FIDO2...");
            SendJob(job);
            break;
        }
        case kResetNo:
            break;
        default:
            sheet_choice = answer;
            break;
    }
    if (nav_state.mode() == nav::Mode::kList && lv_tick_elaps(looked_at) >= kVaultMs) {
        looked_at = lv_tick_get();
        const Seen now = Look();
        changed |= now.state != seen.state || now.count != seen.count ||
                   now.version != seen.version;
    }
    if (settings_tapped >= 0) {
        changed |= nav_state.OpenSettings(static_cast<uint8_t>(settings_tapped));
        settings_tapped = -1;
    }
    if (action_tapped) {
        action_tapped = false;
        Job job = MakeJob(JobKind::kAct);
        job.action = settings::PageAction(static_cast<settings::Page>(nav_state.settings_page()));
        if (job.action == settings::Action::kFidoReset) {
            fido_reset_tapped = true;
        } else if (job.action == settings::Action::kRestart) {
            if (!dialog::IsOpen()) {  // asked first: the vault locks
                const SheetButton buttons[] = {{"Restart", 1}, {"Cancel", 3}};
                OpenSheet("Restart now? The PIN is asked again after it.", buttons, 2);
            }
        } else if (xQueueSend(jobs, &job, 0) != pdTRUE) {
            Toast("busy typing");
        }
    }
    if (sheet_choice != 0 &&
        nav_state.settings_page() == static_cast<uint8_t>(settings::Page::kAbout)) {
        const int choice = sheet_choice;
        sheet_choice = 0;
        CloseSheet();
        if (choice == 1) {
            Job job = MakeJob(JobKind::kAct);
            job.action = settings::Action::kRestart;
            Toast("restarting...");
            SendJob(job);
        }
    }
    if (light_save) {
        light_save = false;
        const Job job = MakeJob(JobKind::kSaveLight);
        xQueueSend(jobs, &job, 0);  // full: the next slider let go saves it
    }
    const auto settings_page = static_cast<settings::Page>(nav_state.settings_page());
    if (nav_state.mode() == nav::Mode::kSettings && wifi_hooks != nullptr) {
        changed |= PollWifi();
    }
    if (nav_state.mode() == nav::Mode::kSettings && wifi_hooks != nullptr) {
        changed |= PollWifiMore();
    }
    if (nav_state.mode() == nav::Mode::kSettings && ble_hooks != nullptr) {
        changed |= PollBle();
    }
    if (nav_state.mode() == nav::Mode::kSettings) {
        changed |= PollTouch();
    }
    if (nav_state.mode() == nav::Mode::kSettings && display_hooks != nullptr) {
        changed |= PollDisplay();
    }
    if (nav_state.mode() == nav::Mode::kSettings && time_hooks != nullptr) {
        changed |= PollTime();
    }
    if (nav_state.mode() == nav::Mode::kSettings && security_hooks != nullptr) {
        changed |= PollSecurity();
    }
    if (nav_state.mode() == nav::Mode::kSettings && web_hooks != nullptr) {
        changed |= PollWeb();
    }
    if (nav_state.mode() == nav::Mode::kSettings) {
        changed |= PollFidoSettings();
    }
    // Settings' texts (Wi-Fi joined, the clock); the switch follows Power x2.
    // Not under a dialog, nor over a password being typed or a scan.
    if (nav_state.mode() == nav::Mode::kSettings && settings_hooks != nullptr && HomeShown() &&
        !FingerDown() && !dialog::IsOpen() && settings_page != settings::Page::kWifiPassword &&
        settings_page != settings::Page::kWifiScan && settings_page != settings::Page::kBlePair &&
        settings_page != settings::Page::kTouch &&
        settings_page != settings::Page::kDisplayChoice &&
        settings_page != settings::Page::kTimeSync && settings_page != settings::Page::kTimeRegion &&
        settings_page != settings::Page::kTimeZone && settings_page != settings::Page::kTimeSet &&
        settings_page != settings::Page::kSecurityLock &&
        settings_page != settings::Page::kSecurityPin && settings_page != settings::Page::kAbout &&
        settings_page != settings::Page::kWebMode && settings_page != settings::Page::kWebLogin &&
        settings_page != settings::Page::kBleName && settings_page != settings::Page::kWifiMore &&
        settings_page != settings::Page::kWifiChoice && settings_page != settings::Page::kWifiText &&
        settings_page != settings::Page::kWifiStatic &&
        settings_page != settings::Page::kWifiStaticText &&
        settings_page != settings::Page::kWifiHidden &&
        lv_tick_elaps(settings_looked_at) >= kSettingsMs) {
        settings_looked_at = lv_tick_get();
        if (settings_page == settings::Page::kWifi && wifi_hooks != nullptr) {
            wifi_hooks->view(&wifi_now);
            changed |= !wifi_view::Equal(wifi_now, wifi_shown);
        } else if (settings_page == settings::Page::kBluetooth && ble_hooks != nullptr) {
            ble_hooks->view(&ble_now);
            changed |= !ble_view::Equal(ble_now, ble_shown);
        } else if (settings_page == settings::Page::kWeb && web_hooks != nullptr) {
            web_hooks->view(&web_now);
            changed |= !web_view::Equal(web_now, web_shown);
        } else if (settings_page == settings::Page::kSecurity && security_hooks != nullptr) {
            security_hooks->view(&security_now);
            changed |= !security_view::Equal(security_now, security_shown);
        } else if (settings_page == settings::Page::kTime && time_hooks != nullptr) {
            time_hooks->view(&time_now);
            changed |= !time_view::Equal(time_now, time_shown);
        } else if (settings_page == settings::Page::kDisplay && display_hooks != nullptr) {
            display_hooks->view(&display_now);
            changed |= !display_view::Equal(display_now, display_shown);
        } else {
            SettingsText(settings_page, settings_now, sizeof settings_now);
            changed |= strcmp(settings_now, settings_shown) != 0;
        }
        bool on = false;
        uint8_t brightness = 0;
        uint8_t warmth = 0;
        if (light_switch != nullptr && settings_hooks->light(&on, &brightness, &warmth) &&
            on != lv_obj_has_state(light_switch, LV_STATE_CHECKED)) {
            if (on) {
                lv_obj_add_state(light_switch, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(light_switch, LV_STATE_CHECKED);
            }
        }
    }
    // The store's lock is held while a request waits for the button.
    if (nav_state.mode() == nav::Mode::kFido && !fido_wait && HomeShown() &&
        lv_tick_elaps(fido_looked_at) >= kFidoMs) {
        fido_looked_at = lv_tick_get();
        const FidoSeen now = FidoLook();
        changed |= now.err != fido_seen.err || now.count != fido_seen.count ||
                   now.version != fido_seen.version;
    }
    if (changed) {
        Show();
        return;
    }
    const time_t now = time(nullptr);
    if (row_count > 0 && list::Window(now) != code_window) {
        UpdateCodes(now);
    }
    for (uint8_t i = 0; i < row_count; ++i) {
        if (rows[i].shown && lv_tick_elaps(rows[i].shown_at) >= list::kRevealMs) {
            HidePassword(rows[i]);
        }
    }
    if (!lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN) && lv_tick_elaps(toast_at) >= kToastMs) {
        lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

// From ui.cpp, under Lock: on the screen shown at boot.
void StartScreens() {
    inputs = xQueueCreateStatic(kQueueDepth, 1, inputs_buffer, &inputs_storage);
    jobs = xQueueCreateStatic(sizeof jobs_buffer / sizeof(Job), sizeof(Job), jobs_buffer,
                              &jobs_storage);
    xTaskCreateStatic(&TypeTask, "ui_type", kTypeStackBytes, nullptr, 2, type_stack,
                      &type_storage);
    home = lv_screen_active();
    lv_obj_set_style_bg_color(home, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_obj_remove_flag(home, LV_OBJ_FLAG_SCROLLABLE);

    content = lv_obj_create(home);
    lv_obj_remove_style_all(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(content, kLeft, kContentTop);
    lv_obj_set_size(content, kRowWidth, kContentBottom - kContentTop);

    StartTabs(home);
    dialog::Start(home, kContentTop, kRowWidth - 32);
    StartBar();
    Show();
    UpdateStatus(nullptr);
    lv_timer_create(Poll, kPollMs, nullptr);
    lv_timer_create(UpdateStatus, kStatusMs, nullptr);
}

void SetStatusSource(void (*fn)(status::Status *out)) { source = fn; }

void SetFidoDemo(uint8_t count) {
    Lock();
    fido_demo = count;
    fido_seen.err = ESP_FAIL;  // drawn again at the next look
    Unlock();
}

void ShowScreen(nav::Mode mode) {
    Lock();
    open_mode = static_cast<int>(mode);
    Unlock();
}

void SetTypist(esp_err_t (*type)(const char *text)) { typist = type; }

void SetSettingsHooks(const SettingsHooks *hooks) { settings_hooks = hooks; }

void SetWifiHooks(const WifiHooks *hooks) { wifi_hooks = hooks; }

void SetBleHooks(const BleHooks *hooks) { ble_hooks = hooks; }

void SetDisplayHooks(const DisplayHooks *hooks) { display_hooks = hooks; }

void SetTimeHooks(const TimeHooks *hooks) { time_hooks = hooks; }

void SetSecurityHooks(const SecurityHooks *hooks) { security_hooks = hooks; }

void SetWebHooks(const WebHooks *hooks) { web_hooks = hooks; }

void SetFidoRequest(const fido_prompt::Request &request) {
    Lock();
    fido_request = request;
    Unlock();
}

void ShowFidoPromptDemo(bool on, uint8_t accounts) {
    Lock();
    if (on) {
        fido_request = {};
        fido_request.kind = fido_prompt::Kind::kSignIn;
        snprintf(fido_request.site, sizeof fido_request.site, "github.com");
        if (accounts <= 1) {
            fido_prompt::AddAccount(&fido_request, "octocat", "The Octocat");
        }
        for (unsigned i = 1; accounts > 1 && i <= accounts; ++i) {
            char user[16];
            snprintf(user, sizeof user, "octocat%u", i);
            fido_prompt::AddAccount(&fido_request, user, "");
        }
        fido_request.verify = true;
    }
    prompt_demo = on;
    HidePrompt();  // drawn again (or not) at the next status look
    Unlock();
}

void ShowUpdate(const char *version, const char *source, uint32_t written, uint32_t total) {
    if (!HasDisplay()) {
        return;
    }
    const uint8_t percent = dialog::UpdatePercent(written, total);
    Lock();
    if (update_box == nullptr) {
        OpenUpdate();
    }
    if (dialog::UpdateRedraw(update_shown, percent)) {
        lv_label_set_text_fmt(update_line, "%s%s%s", version[0] != '\0' ? version : "checking",
                              source[0] != '\0' ? " from the " : "",
                              std::strcmp(source, "sd") == 0 ? "SD card" : source);
        DrawUpdatePercent(percent);
    }
    Unlock();
}

void EndUpdate(const char *error) {
    if (!HasDisplay()) {
        return;
    }
    Lock();
    if (update_box == nullptr) {
        OpenUpdate();
    }
    if (error == nullptr) {
        lv_label_set_text(update_title, "Installed");
        lv_label_set_text(update_note, "Restarting into the new firmware.");
        DrawUpdatePercent(100);
    } else {
        lv_label_set_text(update_title, "Not installed");
        lv_label_set_text(update_line, error);
        lv_label_set_text(update_percent, "");
        lv_obj_add_flag(lv_obj_get_parent(update_fill), LV_OBJ_FLAG_HIDDEN);  // no bar to show
        lv_label_set_text(update_note, "The running firmware stays.");
        lv_timer_t *t = lv_timer_create(CloseUpdate, kUpdateFailedMs, nullptr);
        lv_timer_set_repeat_count(t, 1);
    }
    Unlock();
}

void Navigate(nav::Input input) {
    if (inputs != nullptr) {
        const auto in = static_cast<uint8_t>(input);
        xQueueSend(inputs, &in, 0);  // full: dropped (8 presses unread)
    }
}

}  // namespace ui
