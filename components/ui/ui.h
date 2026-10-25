#pragma once

// LVGL v9: 1-bit colour, all memory in PSRAM (lv_mem_psram.c), settings in
// sdkconfig.defaults. The e-ink panel is a 480x800 portrait display
// (ui_logic.h turns it onto the landscape panel), redrawn by the "ui" task
// that runs lv_timer_handler. The GT911 (touch.h, polled by the keypad task)
// is LVGL's pointer, mapped onto the portrait screen (TouchToScreen).

#include "esp_err.h"
#include "nav_logic.h"
#include "settings_logic.h"
#include "status_logic.h"
#include "wifi_view_logic.h"
#include "ble_view_logic.h"
#include "display_view_logic.h"
#include "time_view_logic.h"
#include "security_view_logic.h"
#include "web_view_logic.h"
#include "fido_prompt_logic.h"
#include "touch_cal_logic.h"

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

// A random PNG from /sdcard/wallpapers (wallpaper_logic.h), dithered to 4
// grey levels where the panel has them (`grey` false: B/W), full refresh;
// blocks for seconds. The caller holds Lock, or the ui task may draw over it.
// ESP_ERR_NOT_FOUND: no folder or no PNG in it.
esp_err_t ShowWallpaper(bool grey = true);
// LVGL's frames kept off the panel (a wallpaper on it) or let through again;
// holding waits for a refresh under way. ShowWallpaper holds (power-off
// never lets go); the next LVGL change after a release redraws it all.
void HoldPanel(bool hold);
// Console `ui wallpaper`: ShowWallpaper in a task of its own (under Lock),
// waits for it; `shown` is the file. The device stays on; the next LVGL
// change draws over it.
esp_err_t PreviewWallpaper(bool grey, const char **shown);

// The status bar's source (status_logic.h): called in the ui task every 2 s,
// fills what the board reports. Keep it quick (cache slow reads).
void SetStatusSource(void (*source)(status::Status *out));
// Types a record's user, password or TOTP code on the host (ble_kb::Type),
// called from a task of the ui's own. ESP_ERR_INVALID_STATE: no host.
void SetTypist(esp_err_t (*type)(const char *text));

// What Settings shows and does (settings_logic.h), filled by the device.
struct SettingsHooks {
    // The frontlight's state and level; false: no frontlight. ui task.
    bool (*light)(bool *on, uint8_t *brightness, uint8_t *warmth);
    // Lit (or not) at this level now, not saved. ui task.
    void (*set_light)(bool on, uint8_t brightness, uint8_t warmth);
    // The level into config.json (a slider let go). The ui's own task.
    esp_err_t (*save_light)();
    // A page's "label\tvalue\n" lines, or (summary) the short text on its row
    // in the main page. ui task, every 2 s while shown: keep it quick.
    void (*describe)(settings::Page page, bool summary, char *out, size_t size);
    // A page's button (settings::PageAction); what to say. The ui's own task.
    const char *(*act)(settings::Action action);
};
void SetSettingsHooks(const SettingsHooks *hooks);

// The Wi-Fi window (wifi_view_logic.h), filled and carried out by the device.
struct WifiHooks {
    // What it shows. ui task, every 2 s while shown: keep it quick.
    void (*view)(wifi_view::View *out);
    // The rest in the ui's own task (each may take seconds), saying what
    // happened. The networks nearby: their count (0: none or failed).
    size_t (*scan)(wifi_view::Nearby *out, size_t capacity);
    const char *(*set_active)(bool on);
    // The network saved (with `password`; nullptr: the kept one) and tried
    // first; Wi-Fi on.
    const char *(*join)(const char *ssid, const char *password);
    const char *(*forget)(const char *ssid);
    // A saved network's address (ui task); false: no such network.
    bool (*get_static)(uint8_t index, wifi_view::StaticIp *out);
    // The rest in the ui's own task, saying what happened.
    const char *(*set_choice)(wifi_view::Choice choice, uint16_t value);
    const char *(*set_text)(wifi_view::Field field, const char *value);
    const char *(*set_static)(const char *ssid, const wifi_view::StaticIp &ip);
};
void SetWifiHooks(const WifiHooks *hooks);

// The Bluetooth window (ble_view_logic.h), filled and carried out by the device.
struct BleHooks {
    // What it shows. ui task, every 2 s while shown: keep it quick.
    void (*view)(ble_view::View *out);
    // The pairing window's next event (ble::WaitPairingEvent, not waiting)
    // and its seconds left. ui task.
    bool (*pairing_event)(ble_view::Pairing *what, uint32_t *passkey);
    uint32_t (*pairing_left)();
    // The rest in the ui's own task, saying what happened.
    const char *(*set_active)(bool on);
    const char *(*use)(const char *addr);  // nullptr: any paired host
    const char *(*forget)(const char *addr);
    const char *(*pair)(bool start);  // the pairing window opened / closed
    const char *(*set_name)(const char *name);  // nullptr: the board's
};
void SetBleHooks(const BleHooks *hooks);

// The Display & power window (display_view_logic.h), filled by the device.
struct DisplayHooks {
    // What it shows. ui task, every 2 s while shown: keep it quick.
    void (*view)(display_view::View *out);
    // A choice kept (config.json); what to say. The ui's own task.
    const char *(*set)(display_view::Setting setting, uint16_t value);
};
void SetDisplayHooks(const DisplayHooks *hooks);

// The Date & time window (time_view_logic.h), filled by the device.
struct TimeHooks {
    // What it shows. ui task, every 2 s while shown: keep it quick.
    void (*view)(time_view::View *out);
    // The zone table (zones.h).
    size_t (*zone_count)();
    const char *(*zone_name)(size_t index);
    // The rest in the ui's own task, saying what happened.
    const char *(*set_zone)(const char *name);
    const char *(*set_sync)(uint8_t hours);
    const char *(*set_time)(const time_view::Manual &local);
};
void SetTimeHooks(const TimeHooks *hooks);

// The Security window (security_view_logic.h), filled by the device.
struct SecurityHooks {
    // What it shows. ui task, every 2 s while shown: keep it quick.
    void (*view)(security_view::View *out);
    // The rest in the ui's own task (a PIN change takes seconds), saying what
    // happened. The PINs are wiped by the caller after the call.
    const char *(*change_pin)(const char *old_pin, const char *new_pin);
    const char *(*set_wipe)(const char *pin);  // nullptr: no wipe PIN
    const char *(*set_lock)(uint16_t minutes);
    const char *(*set_complexity)(bool on);
};
void SetSecurityHooks(const SecurityHooks *hooks);

// The Web site window (web_view_logic.h), filled by the device.
struct WebHooks {
    // What it shows. ui task, every 2 s while shown: keep it quick.
    void (*view)(web_view::View *out);
    // The rest in the ui's own task, saying what happened.
    const char *(*set_mode)(uint8_t mode);
    const char *(*set_write)(bool on);
    const char *(*set_login)(const char *user, const char *password);  // nullptr: none
};
void SetWebHooks(const WebHooks *hooks);

// The FIDO2 / U2F request about to run (any task, before the core takes it):
// while the core waits for the button, a window says what it is (the site,
// the account) and which button does what (Left or Home allow, Right refuses).
void SetFidoRequest(const fido_prompt::Request &request);
// Console `ui demo prompt [n]`: the window for a made-up sign-in (n accounts
// for the site), shown (or not) with no request; the buttons do nothing to it.
void ShowFidoPromptDemo(bool on, uint8_t accounts = 1);

// The touch's calibration (touch_cal_logic.h), applied to every real touch;
// saved by `save` (the ui's own task) when Settings > Touchscreen keeps a new one.
void SetTouchCalibration(const touch_cal::Calibration &calibration,
                         esp_err_t (*save)(const touch_cal::Calibration &calibration));
// Left / Right / Home (nav_logic.h), from any task: queued for the ui task;
// nothing while the PIN pad is up.
void Navigate(nav::Input input);

// The LVGL screen as ui_logic.h has it (I1, 1 white, rows of kRowBytes, MSB
// the leftmost): `out` takes kScreenBytes. False: no display.
bool CopyScreen(uint8_t *out);

// Console `ui demo fido <n>`: n made-up keys (up to 30) added to the shown
// U2F / FIDO2 list, only on the screen (the store is not touched); 0 none.
void SetFidoDemo(uint8_t count);
// Console `ui click` / `ui swipe`: a made-up finger from (x0, y0) to (x1, y1)
// (the same point: a tap, held `hold_ms` if given) on the portrait screen,
// played in ~200 ms as the touch would be. ESP_ERR_INVALID_STATE: a finger
// (real or not) is down.
esp_err_t Touch(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t hold_ms = 0);
// Console `ui show`: that screen, as its swipe would open it.
void ShowScreen(nav::Mode mode);

// A firmware update under way (ota_update.h), from any task: a window over
// every screen, the PIN pad too (taps and buttons do nothing), with the
// version, where it comes from and a bar (dialog_logic.h kUpdateStep steps).
void ShowUpdate(const char *version, const char *source, uint32_t written, uint32_t total);
// The end: `error` nullptr: installed, restarting (the window stays);
// else the reason, and the window closes after a few seconds.
void EndUpdate(const char *error);

// Called from the ui task when a finger goes down (power::NoteActivity).
void SetPressHook(void (*hook)());

// "9.4.0".
const char *LvglVersion();

// Console `version`/`ui`: display, refresh count, the last flush, task stack.
void PrintInfo();

}  // namespace ui
