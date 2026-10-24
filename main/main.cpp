#include <sys/stat.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "battery.h"
#include "ble_fido.h"
#include "ble_kb.h"
#include "ble.h"
#include "board_devices.h"
#include "config.h"
#include "console.h"
#include "crypto.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "epd.h"
#include "fido_ctap.h"
#include "frontlight.h"
#include "host/ble_hs.h"
#include "keypad.h"
#include "nvs_flash.h"
#include "power.h"
#include "sd_card.h"
#include "spiffs_fs.h"
#include "store_io.h"
#include "timesync.h"
#include "timezone.h"
#include "ui.h"
#include "vault.h"
#include "web_server.h"
#include "wifi_manager.h"

namespace {

constexpr const char *TAG = "main";

// The stock bootloader has rollback enabled: an app flashed by OTA boots as
// PENDING_VERIFY and is rolled back on the next reset unless it confirms itself.
void ConfirmRunningApp() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "app marked valid, rollback cancelled");
    }
}

void InitNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs: %s, erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// Where store files (fido.bin, vault.bin) live: /sdcard/.authenticator when the
// board has a card slot, else SPIFFS. nullptr when that file system is not
// mounted: no card is no stores, never a second set on SPIFFS.
const char *StorageDir() {
    if (boards::kBoard.has_sd_card) {
        if (!sd_card::Mounted()) {
            ESP_LOGE(TAG, "SD card: %s, no store files", sd_card::Name(sd_card::Status()));
            return nullptr;
        }
        struct stat st = {};
        if (stat(sd_card::kStoreDir, &st) != 0 && mkdir(sd_card::kStoreDir, 0775) != 0) {
            ESP_LOGE(TAG, "SD card: cannot create %s, no store files", sd_card::kStoreDir);
            return nullptr;
        }
        return sd_card::kStoreDir;
    }
    return spiffs_fs::Mounted() ? spiffs_fs::kBasePath : nullptr;
}

// A FIDO2 request waits for the user: Left / Right answer it, not the screens.
std::atomic<bool> fido_waiting{false};

// FIDO2 over BLE: the LionKey CTAP core behind the ble_fido transport. User
// presence: the board's Left button (BOOT on the dev board), debounced by the
// keypad task, which owns the pins; Right refuses.
void InitFido(const char *dir) {
    static char store[64];
    const bool have_fs = dir != nullptr && snprintf(store, sizeof store, "%s/fido.bin", dir) <
                                               static_cast<int>(sizeof store);
    // The only transport today is BLE: the creator of a credential is the host
    // on the link.
    const fido_ctap_hooks_t hooks = {
        ble_fido::Keepalive, ble_fido::Cancelled,
        [] { return keypad::Pressed(boards::ButtonId::kLeft); },
        [](fido_ctap_client_t *out) {
            out->transport = FIDO_CTAP_TRANSPORT_BLE;
            return ble::PeerIdentity(&out->addr_type, out->addr);
        },
        [](fido_ctap_wait_t what) {
            fido_waiting = what != FIDO_CTAP_WAIT_NONE;
            boards::LedWaitingForUser(static_cast<boards::UserWait>(what));
        },
        [] { return config::Get().test.auto_presence; },
        [] { return keypad::Pressed(boards::ButtonId::kRight); }};
    static_assert(static_cast<int>(boards::UserWait::kLongTouchHeld) ==
                  FIDO_CTAP_WAIT_LONG_TOUCH_HELD);
    fido_ctap_set_reset_window(boards::kBoard.fido_reset_window_s);
    fido_ctap_set_pin_complexity(config::Get().pin.complexity);
    if (fido_ctap_init(&hooks, have_fs ? store : nullptr, boards::kBoard.fido_max_credentials) ==
        ESP_OK) {
        // A request (and its wait for the button) is activity: no idle off.
        ble_fido::SetMessageHandler([](const uint8_t *msg, size_t len, uint8_t *out,
                                       size_t out_size) {
            power::NoteActivity();
            const size_t n = fido_ctap_request(msg, len, out, out_size);
            power::NoteActivity();
            return n;
        });
    } else {
        ESP_LOGE(TAG, "fido: CTAP core init failed");
    }
}

fido_ctap_key_t Convert(const vault::Key &k) {
    fido_ctap_key_t out;
    static_assert(sizeof out.key == sizeof k.key && sizeof out.salt == sizeof k.salt &&
                  sizeof out.id == sizeof k.id);
    memcpy(out.key, k.key, sizeof out.key);
    memcpy(out.salt, k.salt, sizeof out.salt);
    out.iterations = k.iterations;
    memcpy(out.id, k.id, sizeof out.id);
    return out;
}

// Passwords: locked until the PIN (console `pin`), unless config.json has a
// test PIN (test.pin, development only).
void InitVault(const char *dir) {
    crypto::Init();
    const config::Pin &cfg = config::Get().pin;
    vault::SetPolicy({static_cast<uint32_t>(cfg.lock_minutes) * 60, cfg.wipe, cfg.complexity});
    // fido.bin has the vault's key: FIDO2 runs while it is in, a PIN change
    // re-keys it, the wipe PIN erases it (quietly: it looks like a wrong PIN).
    vault::SetHooks({
        .key_in = [](const vault::Key &k) {
            const fido_ctap_key_t key = Convert(k);
            fido_ctap_set_key(&key);
        },
        .key_gone = [] { fido_ctap_set_key(nullptr); },
        .stage = [](const vault::Key &k) {
            const fido_ctap_key_t key = Convert(k);
            return fido_ctap_stage_key(&key);
        },
        .commit = [] { fido_ctap_commit_key(); },
        .unstage = [] { fido_ctap_unstage_key(); },
        .wipe = [] { fido_ctap_wipe(); },
    });
    // The key kept through the sleep (power.keepPinHours): handed over at
    // the sleep, taken back at the wake.
    power::SetSleepHook([](bool lock) {
        const uint8_t hours = config::Get().power.keep_pin_hours;
        vault::Key k;
        if (!lock && hours != 0 && vault::CopyKey(&k)) {
            power::Keep(&k, sizeof k, hours);
        }
        vault::Wipe(&k, sizeof k);
    });
    if (vault::Init(dir, boards::kBoard.max_records) != ESP_OK || dir == nullptr) {
        return;
    }
    vault::Key kept;
    if (power::TakeKept(&kept, sizeof kept)) {
        const vault::Error e = vault::Open(kept);
        ESP_LOGI(TAG, "vault: the PIN kept through the sleep: %s", vault::Describe(e));
    }
    vault::Wipe(&kept, sizeof kept);
    const char *pin = config::Get().test.pin;
    if (pin[0] != '\0') {
        const vault::Error e = vault::Unlock(pin);
        ESP_LOGW(TAG, "vault: test.pin from config.json: %s", vault::Describe(e));
    }
}

// The status bar (ui task, every 2 s): the gauge is read every 30 s (I2C),
// the rest is the modules' state.
void StatusSource(ui::status::Status *s) {
    namespace st = ui::status;
    constexpr int64_t kBatteryMs = 30 * 1000;
    static int64_t battery_at = -kBatteryMs;
    static int level = -1;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - battery_at >= kBatteryMs) {
        level = battery::Level();
        battery_at = now_ms;
    }
    s->battery = static_cast<int8_t>(level);
    s->charging = battery::ChargeState() == battery::Charge::kCharging;

    const wifimgr::Snapshot w = wifimgr::Get();
    switch (w.state) {
        case wifimgr::State::kOff: s->wifi = st::Wifi::kOff; break;
        case wifimgr::State::kConnecting:
        case wifimgr::State::kWaiting: s->wifi = st::Wifi::kTrying; break;
        case wifimgr::State::kOnline:
            s->wifi = w.internet == wifimgr::Internet::kOffline ? st::Wifi::kNoInternet
                                                                : st::Wifi::kOnline;
            break;
        case wifimgr::State::kIdle:
        case wifimgr::State::kApWindow:
        case wifimgr::State::kAp: s->wifi = st::Wifi::kAp; break;
    }
    s->ble = !ble::IsOn()                                     ? st::Ble::kOff
             : ble::PairingSecondsLeft() > 0                  ? st::Ble::kPairing
             : ble::ConnHandle() != BLE_HS_CONN_HANDLE_NONE ? st::Ble::kConnected
                                                              : st::Ble::kWaiting;
    s->sd = sd_card::Mounted();
    s->fido_wait = fido_waiting;

    const time_t now = time(nullptr);
    s->time_set = now >= 1704067200;  // 2024-01-01, rtc::Believable's floor
    struct tm local = {};
    localtime_r(&now, &local);
    s->hour = static_cast<uint8_t>(local.tm_hour);
    s->minute = static_cast<uint8_t>(local.tm_min);
}

// Button bindings, from the keypad task; any key is activity. Left / Right /
// Home pressed: the screens (ui::Navigate), unless FIDO2 waits for them. Power x2: the
// frontlight on / off; Power held: off (wallpaper, deep sleep); Power x3: off
// locked (the PIN after the wake, whatever power.keepPinHours says).
void OnKey(const buttons::KeyEvent &e) {
    power::NoteActivity();
    if (e.kind == buttons::Kind::kPressed && !fido_waiting) {
        switch (static_cast<boards::ButtonId>(e.key)) {
            case boards::ButtonId::kLeft: ui::Navigate(ui::nav::Input::kLeft); break;
            case boards::ButtonId::kRight: ui::Navigate(ui::nav::Input::kRight); break;
            case boards::ButtonId::kHome: ui::Navigate(ui::nav::Input::kHome); break;
            default: break;
        }
    }
    if (e.key != static_cast<uint8_t>(boards::ButtonId::kPower)) {
        return;
    }
    if (e.kind == buttons::Kind::kClick && e.count == 2) {
        frontlight::Toggle();
    } else if (e.kind == buttons::Kind::kClick && e.count == 3) {
        power::RequestOff(true);
    } else if (e.kind == buttons::Kind::kLong) {
        power::RequestOff();
    }
}

// --- Settings on the screen (ui::SettingsHooks) ------------------------------------

bool SettingsLight(bool *on, uint8_t *brightness, uint8_t *warmth) {
    if (!frontlight::Present()) {
        return false;
    }
    *on = frontlight::IsOn();
    *brightness = config::Get().frontlight.brightness;
    *warmth = config::Get().frontlight.warmth;
    return true;
}

// The level into the config in memory; saved when a slider is let go.
void SettingsSetLight(bool on, uint8_t brightness, uint8_t warmth) {
    config::Get().frontlight.brightness = brightness;
    config::Get().frontlight.warmth = warmth;
    frontlight::SetLevel(brightness, warmth);
    if (on) {
        frontlight::On();
    } else {
        frontlight::Off();
    }
}

esp_err_t SettingsSaveLight() { return config::Save(); }

// "label\tvalue\n" added to `out`.
void Put(char *out, size_t size, const char *label, const char *format, ...) {
    const size_t used = strlen(out);
    if (used + 1 >= size) {
        return;
    }
    int n = snprintf(out + used, size - used, "%s\t", label);
    if (n < 0 || used + n >= size) {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(out + used + n, size - used - n, format, args);
    va_end(args);
    const size_t end = strlen(out);
    if (end + 1 < size) {
        out[end] = '\n';
        out[end + 1] = '\0';
    }
}

void Ip(uint32_t ip, char *out, size_t size) {
    snprintf(out, size, "%u.%u.%u.%u", static_cast<unsigned>(ip & 0xff),
             static_cast<unsigned>((ip >> 8) & 0xff), static_cast<unsigned>((ip >> 16) & 0xff),
             static_cast<unsigned>(ip >> 24));
}

void DescribeWifi(bool summary, char *out, size_t size) {
    const wifimgr::Snapshot w = wifimgr::Get();
    const bool online = w.state == wifimgr::State::kOnline;
    const bool ap = w.state == wifimgr::State::kAp || w.state == wifimgr::State::kApWindow ||
                    w.state == wifimgr::State::kIdle;
    const char *state = w.state == wifimgr::State::kOff ? "off"
                        : online ? (w.internet == wifimgr::Internet::kOffline ? "no internet"
                                                                              : "online")
                        : ap     ? "access point"
                                 : "connecting";
    if (summary) {
        snprintf(out, size, "%s", online ? w.radio.ssid : ap ? w.radio.ap_ssid : state);
        return;
    }
    char ip[16];
    Put(out, size, "State", "%s", state);
    if (online) {
        Ip(w.radio.ip, ip, sizeof ip);
        Put(out, size, "Network", "%s", w.radio.ssid);
        Put(out, size, "Address", "%s%s", ip, w.radio.ip_is_static ? " (static)" : "");
        Put(out, size, "Signal", "%d dBm", w.radio.rssi);
    }
    if (w.radio.ap_ssid[0] != '\0' && ap) {
        Ip(w.radio.ap_ip, ip, sizeof ip);
        Put(out, size, "Access point", "%s", w.radio.ap_ssid);
        Put(out, size, "AP address", "%s", ip);
    }
    unsigned saved = 0;
    for (const config::Network &n : config::Get().wifi.networks) {
        saved += n.ssid[0] != '\0' ? 1 : 0;
    }
    Put(out, size, "Saved networks", "%u", saved);
}

void DescribeBluetooth(bool summary, char *out, size_t size) {
    const bool on = ble::IsOn();
    const bool connected = on && ble::ConnHandle() != BLE_HS_CONN_HANDLE_NONE;
    const uint32_t pairing = on ? ble::PairingSecondsLeft() : 0;
    const char *state = !on ? "off" : pairing > 0 ? "pairing" : connected ? "connected" : "waiting";
    if (summary) {
        snprintf(out, size, "%s", state);
        return;
    }
    Put(out, size, "State", "%s", state);
    Put(out, size, "Name", "%s", ble::Name());
    uint8_t type = 0;
    uint8_t addr[6] = {};
    if (connected && ble::PeerIdentity(&type, addr)) {
        Put(out, size, "Host", "%02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1], addr[2],
            addr[3], addr[4], addr[5]);
        Put(out, size, "Link", "%s", ble::LinkSecure() ? "encrypted" : "not encrypted");
    }
    int bonds = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &bonds);
    Put(out, size, "Paired hosts", "%d", bonds);
    if (pairing > 0) {
        Put(out, size, "Pairing for", "%u s", static_cast<unsigned>(pairing));
    }
}

void DescribeDisplay(bool summary, char *out, size_t size) {
    const config::Power &p = config::Get().power;
    if (summary) {
        if (p.idle_minutes == 0) {
            snprintf(out, size, "never off");
        } else {
            snprintf(out, size, "off after %u min", static_cast<unsigned>(p.idle_minutes));
        }
        return;
    }
    const int level = battery::Level();
    if (level >= 0) {
        Put(out, size, "Battery", "%d %%%s", level,
            battery::ChargeState() == battery::Charge::kCharging ? ", charging" : "");
    }
    if (p.idle_minutes == 0) {
        Put(out, size, "Power off", "never on its own");
    } else {
        Put(out, size, "Power off", "after %u min idle", static_cast<unsigned>(p.idle_minutes));
    }
    if (p.keep_pin_hours == 0) {
        Put(out, size, "PIN after sleep", "always");
    } else {
        Put(out, size, "PIN after sleep", "after %u h", static_cast<unsigned>(p.keep_pin_hours));
    }
    Put(out, size, "Wallpaper", "%s",
        config::Get().display.wallpaper == config::WallpaperMode::kGrey ? "grey" : "black & white");
}

void DescribeSecurity(bool summary, char *out, size_t size) {
    const config::Pin &pin = config::Get().pin;
    if (summary) {
        snprintf(out, size, "%s", pin.wipe[0] != '\0' ? "wipe PIN set" : "PIN");
        return;
    }
    const vault::Info v = vault::GetInfo();
    Put(out, size, "Vault", "%s, %u records", vault::StateName(v.state),
        static_cast<unsigned>(v.count));
    fido_ctap_info_t f = {};
    // Not while a request waits for the button (the store's lock is taken).
    if (!fido_waiting && fido_ctap_info(&f) == ESP_OK) {
        Put(out, size, "FIDO2 keys", "%u of %u", static_cast<unsigned>(f.credentials),
            static_cast<unsigned>(f.max_credentials));
        Put(out, size, "FIDO2 PIN", "%s", f.pin_set ? "set" : "not set");
    }
    Put(out, size, "Wrong PINs", "%u of %u", static_cast<unsigned>(v.pin_failures),
        static_cast<unsigned>(vault::kMaxPinTries));
    Put(out, size, "Then locked", "%u min", static_cast<unsigned>(pin.lock_minutes));
    Put(out, size, "Wipe PIN", "%s", pin.wipe[0] != '\0' ? "set" : "not set");
    Put(out, size, "PIN rules", "%s", pin.complexity ? "complexity on" : "6 digits");
}

void DescribeTime(bool summary, char *out, size_t size) {
    const config::Time &t = config::Get().time;
    if (summary) {
        snprintf(out, size, "%s", t.zone[0] != '\0' ? t.zone : "UTC");
        return;
    }
    const time_t now = time(nullptr);
    if (now >= 1704067200) {  // 2024-01-01, rtc::Believable's floor
        struct tm local = {};
        localtime_r(&now, &local);
        char text[24];
        strftime(text, sizeof text, "%H:%M", &local);
        Put(out, size, "Time", "%s", text);
        strftime(text, sizeof text, "%Y-%m-%d", &local);
        Put(out, size, "Date", "%s", text);
    } else {
        Put(out, size, "Time", "not set");
    }
    Put(out, size, "Zone", "%s", t.zone[0] != '\0' ? t.zone : "UTC");
    Put(out, size, "Server", "%s", t.server[0] != '\0' ? t.server : "none");
    const timesync::Status s = timesync::Get();
    if (s.syncing) {
        Put(out, size, "Sync", "now");
    } else if (s.ever_synced) {
        Put(out, size, "Last sync", "%u min ago", static_cast<unsigned>(s.since_last_ms / 60000));
    } else {
        Put(out, size, "Last sync", "never");
    }
}

void DescribeAbout(bool summary, char *out, size_t size) {
    const esp_app_desc_t *app = esp_app_get_description();
    if (summary) {
        snprintf(out, size, "%s", app->version);
        return;
    }
    Put(out, size, "Firmware", "%s", app->version);
    Put(out, size, "Built", "%s", app->date);
    Put(out, size, "LVGL", "%s", ui::LvglVersion());
    Put(out, size, "Board", "%s", boards::kBoard.name);
    Put(out, size, "SD card", "%s", sd_card::Mounted() ? "in" : "none");
    Put(out, size, "Free memory", "%u KB",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    Put(out, size, "Free PSRAM", "%u KB",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void SettingsDescribe(ui::settings::Page page, bool summary, char *out, size_t size) {
    using ui::settings::Page;
    out[0] = '\0';
    switch (page) {
        case Page::kWifi: DescribeWifi(summary, out, size); break;
        case Page::kBluetooth: DescribeBluetooth(summary, out, size); break;
        case Page::kDisplay: DescribeDisplay(summary, out, size); break;
        case Page::kSecurity: DescribeSecurity(summary, out, size); break;
        case Page::kTime: DescribeTime(summary, out, size); break;
        case Page::kAbout: DescribeAbout(summary, out, size); break;
        case Page::kMain: break;
    }
}

const char *SettingsAct(ui::settings::Action action) {
    using ui::settings::Action;
    switch (action) {
        case Action::kPowerOff:
            return power::RequestOff() == ESP_OK ? "powering off" : "cannot power off";
        case Action::kLockNow:
            vault::Lock();
            return "locked";
        case Action::kSyncTime:
            timesync::SyncNow();
            return config::Get().time.server[0] != '\0' ? "syncing the time"
                                                        : "no time server set";
        case Action::kNone: break;
    }
    return "";
}

const ui::SettingsHooks kSettingsHooks = {SettingsLight, SettingsSetLight, SettingsSaveLight,
                                          SettingsDescribe, SettingsAct};

}  // namespace

extern "C" void app_main() {
    power::Boot();  // the pins latched through the last deep sleep, released
    boards::Init();  // LED red; the board's clock seeds the system clock (UTC)
    battery::Init();  // not fatal: `battery` reports a missing gauge
    sd_card::Init();  // not fatal: `sd` says why it is not mounted
    keypad::AddListener(OnKey);  // bindings first: the task reads the list unlocked
    keypad::Start();  // the buttons: events, and Left for FIDO2 presence
    ConfirmRunningApp();
    InitNvs();
    spiffs_fs::Init();  // not fatal: the console reports an unmounted fs
    // config.json from SPIFFS; built-in defaults if missing/invalid or unmounted.
    // TODO: look on the microSD card first.
    config::Init(spiffs_fs::kBasePath);
    tz::ApplyZone(config::Get().time.zone);
    // No name in config.json: the board's, put into the config so a save keeps it.
    config::Ble &ble_cfg = config::Get().ble;
    if (ble_cfg.name[0] == '\0') {
        snprintf(ble_cfg.name, sizeof ble_cfg.name, "%s", boards::kBoard.ble_name);
    }
    ble::SetName(ble_cfg.name);
    epd::Init();  // finds the panel's controller; draws nothing (the picture stays)
    // Off at boot; config.json keeps the level a double press of Power lights.
    if (frontlight::Init() == ESP_OK) {
        frontlight::SetLevel(config::Get().frontlight.brightness, config::Get().frontlight.warmth);
    }
    boards::SetLedBrightness(config::Get().led.percent, config::Get().led.idle_percent);

    ui::Init();  // LVGL on the panel (portrait), the boot screen
    ui::SetPressHook(power::NoteActivity);
    ui::SetStatusSource(StatusSource);
    ui::SetTypist(ble_kb::Type);
    ui::SetSettingsHooks(&kSettingsHooks);

    ble_kb::Register();  // BLE profiles, before the stack starts
    ble_kb::SetTypingHook([] {
        boards::LedTyping();
        power::NoteActivity();
    });
    ble_fido::Register();
    const char *dir = StorageDir();
    if (dir != nullptr) {
        // A PIN change cut short: finished or dropped before anything loads.
        static char marker[64];
        static char vault_path[64];
        static char fido_path[64];
        snprintf(marker, sizeof marker, "%s/%s", dir, vault::kPinChangeMarker);
        snprintf(vault_path, sizeof vault_path, "%s/vault.bin", dir);
        snprintf(fido_path, sizeof fido_path, "%s/fido.bin", dir);
        const char *const paths[] = {vault_path, fido_path};
        storefile::FinishCommit(marker, paths, 2);
    }
    InitFido(dir);
    InitVault(dir);
    ESP_ERROR_CHECK(console::Init());

    // The web server reconciles on the Wi-Fi manager's tick (hook first); the
    // manager task follows config.json (radio off unless wifi.active).
    web::SetRequestHook(power::NoteActivity);
    ESP_ERROR_CHECK(web::Init());
    ESP_ERROR_CHECK(wifimgr::Init());
    // SNTP when the internet is up; the result goes back to the board's clock.
    ESP_ERROR_CHECK(timesync::Init(&boards::Clock()));
    if (config::Get().ble.active) {
        ble::On();
    }
    power::Start();  // the idle watch (power.idleMinutes)
    boards::BootDone();  // LED: ready
}
