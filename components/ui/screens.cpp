// The screens behind the PIN pad (x4-design.md): the status bar on LVGL's top
// layer (over every screen, the pad's too), the password list in pages with a
// tab per page at the bottom, Settings pulled down from the top (a stub for
// now) and the U2F / FIDO2 keys pulled up from the bottom. nav_logic.h,
// list_logic.h, fido_list_logic.h and status_logic.h decide; this draws. Typing runs in a task
// of its own (ui::SetTypist; a password takes a second over BLE).

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_app_desc.h"
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
#include "ui.h"
#include "ui_logic.h"
#include "vault.h"

namespace ui {

nav::Gesture TakeGesture();  // ui.cpp
bool LastWasTap();

namespace {

constexpr uint32_t kStatusMs = 500;   // the bar's source, polled (FIDO2's prompt)
constexpr uint32_t kPollMs = 30;      // buttons, swipes, the vault, the codes
constexpr uint32_t kToastMs = 3000;
constexpr uint32_t kVaultMs = 500;    // the vault's changes, looked at
constexpr uint32_t kFidoMs = 2000;    // the FIDO2 store's, while its list is shown
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

struct Job {
    uint32_t id;
    list::Zone zone;
};
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

void UpdateStatus(lv_timer_t *) {
    if (source == nullptr) {
        return;
    }
    status::Status s;
    source(&s);
    fido_wait = s.fido_wait;
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
    if (zone == list::Zone::kShowPassword) {
        if (r.shown) {
            HidePassword(r);
        } else {
            ShowPassword(r);
        }
        return;
    }
    if (zone == list::Zone::kTypeTotp && !r.has_otp) {
        Toast("no TOTP for this record");
        return;
    }
    const Job job = {r.id, zone};
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

constexpr int32_t kFidoRowHeight =
    (kScreenBottom - kContentTop - kTitleHeight) / fido_list::kRowsPerPage;

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
        const Demo &d = kDemo[i % kKinds];
        fido_ctap_credential_t &c = fido_keys[(*count)++];
        memset(&c, 0, sizeof c);
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

void AddFidoRow(uint8_t i, const fido_ctap_credential_t &c) {
    lv_obj_t *row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
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
    lv_obj_set_pos(site, 12, 8);

    lv_obj_t *user = Label(row, &lv_font_montserrat_24);
    lv_label_set_long_mode(user, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(user, kRowWidth - 24, kUserLine);
    fido_list::User(c.user_name, c.display_name, text, sizeof text);
    lv_label_set_text(user, text);
    lv_obj_set_pos(user, 12, 46);

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
    lv_obj_set_pos(details, 12, 80);
}

// The title bar says how many keys and the page; Left / Right flip the pages.
void ShowFido() {
    size_t count = 0;
    // While a request waits for the button, the store's lock is taken.
    const esp_err_t err = fido_wait ? ESP_ERR_TIMEOUT : FidoLoad(&count, &fido_seen);
    nav_state.SetFidoPages(nav::PagesFor(count, fido_list::kRowsPerPage));
    char right[32] = "";
    if (err == ESP_OK && count > 0) {
        if (nav_state.fido_pages() > 1) {
            snprintf(right, sizeof right, "%u   %u/%u", static_cast<unsigned>(count),
                     nav_state.fido_page() + 1u, static_cast<unsigned>(nav_state.fido_pages()));
        } else {
            snprintf(right, sizeof right, "%u", static_cast<unsigned>(count));
        }
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
    }
}

// A tab per page, at most nav::kTabs; arrows where more pages are hidden.
void ShowTabs() {
    const uint8_t n = nav_state.tabs();
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
    lv_obj_clean(content);
    lv_obj_set_height(content, (nav_state.mode() == nav::Mode::kList ? kContentBottom
                                                                       : kScreenBottom) -
                                   kContentTop);
    char text[160];
    switch (nav_state.mode()) {
        case nav::Mode::kList:
            ShowList();
            break;
        case nav::Mode::kSettings:
            TitleBar("Settings");
            snprintf(text, sizeof text,
                     "Frontlight, Wi-Fi and Bluetooth come here.\n\nfirmware %s\nLVGL %s\n\n"
                     "Back: the arrow, a swipe up or Home.",
                     esp_app_get_description()->version, LvglVersion());
            Stub(nullptr, text, kTitleHeight + 24);
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

void TypeTask(void *) {
    for (;;) {
        Job job = {};
        xQueueReceive(jobs, &job, portMAX_DELAY);
        const char *said = TypeJob(job);
        vault::Wipe(&type_record, sizeof type_record);
        Lock();
        Toast(said);
        Unlock();
    }
}

// --- the ui task's timers ------------------------------------------------------------

// Buttons, swipes, the vault's changes, the codes, the hidden passwords.
void Poll(lv_timer_t *) {
    uint8_t in = 0;
    bool changed = false;
    while (xQueueReceive(inputs, &in, 0) == pdTRUE) {
        if (HomeShown()) {  // under the PIN pad the buttons do nothing here
            changed |= nav_state.Apply(static_cast<nav::Input>(in));
        }
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
    if (g != nav::Gesture::kNone && HomeShown()) {
        changed |= nav_state.Swipe(g);
    }
    if (nav_state.mode() == nav::Mode::kList && lv_tick_elaps(looked_at) >= kVaultMs) {
        looked_at = lv_tick_get();
        const Seen now = Look();
        changed |= now.state != seen.state || now.count != seen.count ||
                   now.version != seen.version;
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

void Navigate(nav::Input input) {
    if (inputs != nullptr) {
        const auto in = static_cast<uint8_t>(input);
        xQueueSend(inputs, &in, 0);  // full: dropped (8 presses unread)
    }
}

}  // namespace ui
