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
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "epd.h"
#include "fido_ctap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "frontlight.h"
#include "host/ble_hs.h"
#include "keypad.h"
#include "nvs_flash.h"
#include "ota_update.h"
#include "power.h"
#include "sd_card.h"
#include "server.h"
#include "spiffs_fs.h"
#include "store_io.h"
#include "timesync.h"
#include "timezone.h"
#include "clock_math.h"
#include "ui.h"
#include "usb.h"
#include "usb_fido.h"
#include "vault.h"
#include "web_server.h"
#include "wifi_manager.h"

namespace {

constexpr const char *TAG = "main";

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

// What a request asks, for the prompt on the screen (ui::SetFidoRequest),
// before the core takes it: a sign-in also counts the store's keys it may use
// (those in the allowList, else the site's; the core is idle now, its lock
// free) and names the one there is, or the first few. BLE and USB requests
// may come together: one at a time here.
void PromptFor(const uint8_t *msg, size_t len) {
    namespace fp = ui::fido_prompt;
    static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    static EXT_RAM_BSS_ATTR fp::Request request;  // ~800 bytes: off the transport task's stack
    xSemaphoreTake(lock, portMAX_DELAY);
    request = fp::Parse(msg, len);
    if (request.kind == fp::Kind::kSignIn || request.kind == fp::Kind::kU2fSignIn) {
        static fido_ctap_credential_t *keys = nullptr;  // PSRAM, the store's size
        static size_t capacity = 0;
        fido_ctap_info_t info = {};
        if (fido_ctap_info(&info) == ESP_OK && capacity < info.max_credentials) {
            heap_caps_free(keys);
            keys = static_cast<fido_ctap_credential_t *>(heap_caps_malloc(
                info.max_credentials * sizeof *keys, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            capacity = keys != nullptr ? info.max_credentials : 0;
        }
        size_t count = 0;
        if (keys != nullptr && fido_ctap_list(keys, capacity, &count) == ESP_OK) {
            for (size_t i = 0; i < count; ++i) {
                if (fp::Matches(request, keys[i].rp_id, keys[i].id)) {
                    fp::AddAccount(&request, keys[i].user_name, keys[i].display_name);
                }
            }
        }
    }
    ui::SetFidoRequest(request);
    xSemaphoreGive(lock);
}

// FIDO2: the LionKey CTAP core behind two transports, BLE (ble_fido) and USB
// HID (usb_fido, usb.mode serial+fido); one request at a time (the core's
// lock), the hooks go to the transport whose worker runs it. User presence:
// the board's Left button (BOOT on the dev board) or Home (the touch key
// under the glass), debounced by the keypad task, which owns the pins; Right
// refuses.
const char *fido_file = nullptr;  // the store file (the sync), nullptr: none

void InitFido(const char *dir) {
    static char store[64];
    const bool have_fs = dir != nullptr && snprintf(store, sizeof store, "%s/fido.bin", dir) <
                                               static_cast<int>(sizeof store);
    fido_file = have_fs ? store : nullptr;
    // The creator of a credential: the host on the BLE link, or "USB".
    const fido_ctap_hooks_t hooks = {
        [](uint8_t status) {
            if (usb::fido::InWorker()) {
                usb::fido::Keepalive(status);
            } else {
                ble_fido::Keepalive(status);
            }
        },
        [] { return usb::fido::InWorker() ? usb::fido::Cancelled() : ble_fido::Cancelled(); },
        [] {
            return keypad::Pressed(boards::ButtonId::kLeft) ||
                   keypad::Pressed(boards::ButtonId::kHome);
        },
        [](fido_ctap_client_t *out) {
            if (usb::fido::InWorker()) {
                out->transport = FIDO_CTAP_TRANSPORT_USB;
                return true;  // no address
            }
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
        const auto handler = [](const uint8_t *msg, size_t len, uint8_t *out, size_t out_size) {
            power::NoteActivity();
            PromptFor(msg, len);
            const size_t n = fido_ctap_request(msg, len, out, out_size);
            power::NoteActivity();
            return n;
        };
        ble_fido::SetMessageHandler(handler);
        usb::fido::SetMessageHandler(handler);
    } else {
        ESP_LOGE(TAG, "fido: CTAP core init failed");
    }
}

fido_ctap_key_t Convert(const vault::Key &k) {
    fido_ctap_key_t out;
    static_assert(sizeof out.key == sizeof k.key && sizeof out.salt == sizeof k.salt &&
                  sizeof out.id == sizeof k.id && sizeof out.seed_id == sizeof k.seed_id);
    memcpy(out.key, k.key, sizeof out.key);
    memcpy(out.salt, k.salt, sizeof out.salt);
    out.iterations = k.iterations;
    memcpy(out.id, k.id, sizeof out.id);
    memcpy(out.seed_id, k.seed_id, sizeof out.seed_id);
    return out;
}

// The seed phrase (vault.h): on SPIFFS, never on the card with the stores.
constexpr const char *kSeedPath = "/spiffs/seed.txt";

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
        .adopt = [](const char *from, const vault::Key &k, bool force) {
            const fido_ctap_key_t key = Convert(k);
            return fido_ctap_adopt(from, &key, force);
        },
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
    if (vault::Init(dir, boards::kBoard.max_records, kSeedPath) != ESP_OK || dir == nullptr) {
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

// The status bar (ui task, every 0.5 s): the gauge is read every 30 s (I2C),
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
    if (config::Get().server.host[0] == '\0') {
        s->sync = st::Sync::kHidden;
    } else {
        const server::Status srv = server::Get();
        s->firmware = srv.registered && srv.firmware_newer;
        switch (srv.icon) {
            case server::sync::Icon::kHidden: s->sync = st::Sync::kHidden; break;
            case server::sync::Icon::kOff: s->sync = st::Sync::kOff; break;
            case server::sync::Icon::kPending: s->sync = st::Sync::kPending; break;
            case server::sync::Icon::kOk: s->sync = st::Sync::kOk; break;
            case server::sync::Icon::kConflict: s->sync = st::Sync::kConflict; break;
        }
    }
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
    // The first two: the name and the model (About shows them big).
    Put(out, size, "Name", "%s", ble::Name());
    Put(out, size, "Model", "%s", boards::kBoard.name);
    Put(out, size, "Firmware", "%s", app->version);
    Put(out, size, "Built", "%s %s", app->date, app->time);
    Put(out, size, "ESP-IDF", "%s", esp_get_idf_version());
    Put(out, size, "LVGL", "%s", ui::LvglVersion());
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    Put(out, size, "Chip", "ESP32-S3 v%u.%u, %u cores", static_cast<unsigned>(chip.revision / 100),
        static_cast<unsigned>(chip.revision % 100), static_cast<unsigned>(chip.cores));
    uint32_t flash = 0;
    esp_flash_get_size(nullptr, &flash);
    Put(out, size, "Flash, PSRAM", "%u MB, %u MB", static_cast<unsigned>(flash >> 20),
        static_cast<unsigned>(esp_psram_get_size() >> 20));
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        Put(out, size, "Wi-Fi MAC", "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
            mac[4], mac[5]);
    }
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        Put(out, size, "Bluetooth MAC", "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
            mac[3], mac[4], mac[5]);
    }
    const wifimgr::Snapshot w = wifimgr::Get();
    if (w.state == wifimgr::State::kOnline) {
        char ip[16];
        Ip(w.radio.ip, ip, sizeof ip);
        Put(out, size, "Web site", "http://%s", ip);
    }
    const unsigned minutes = static_cast<unsigned>(esp_timer_get_time() / 60000000);
    Put(out, size, "Up", "%u h %u min", minutes / 60, minutes % 60);
    Put(out, size, "Free memory", "%u KB, PSRAM %u KB",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    Put(out, size, "SD card", "%s", sd_card::Mounted() ? "in" : "none");
}

// Security > FIDO2 / U2F: the state first (the page shows it big).
void DescribeFido(bool summary, char *out, size_t size) {
    fido_ctap_info_t f = {};
    // Not while a request waits for the button (the store's lock is taken).
    const esp_err_t err = fido_waiting ? ESP_ERR_TIMEOUT : fido_ctap_info(&f);
    if (summary) {
        if (err == ESP_OK) {
            snprintf(out, size, "%u keys, PIN %s", static_cast<unsigned>(f.credentials),
                     f.pin_set ? "set" : "not set");
        } else {
            snprintf(out, size, "%s", err == ESP_ERR_TIMEOUT ? "busy" : "stopped");
        }
        return;
    }
    if (err != ESP_OK) {
        const char *why = fido_ctap_stopped_reason();
        Put(out, size, "State", "%s",
            err == ESP_ERR_TIMEOUT ? "Busy: a request waits for the button"
            : why != nullptr       ? why
                                   : "Not running (the vault is locked?)");
        return;
    }
    Put(out, size, "State", "Running over Bluetooth");
    Put(out, size, "Keys", "%u of %u, %u resident", static_cast<unsigned>(f.credentials),
        static_cast<unsigned>(f.max_credentials), static_cast<unsigned>(f.discoverable));
    if (f.pin_set) {
        Put(out, size, "PIN", "set, %u tries left", static_cast<unsigned>(f.pin_retries));
    } else {
        Put(out, size, "PIN", "not set (the site sets one)");
    }
    Put(out, size, "Shortest PIN", "%u", static_cast<unsigned>(f.min_pin_length));
    Put(out, size, "PIN complexity", "%s", f.pin_complexity ? "on" : "off");
    if (f.force_pin_change) {
        Put(out, size, "PIN change", "due");
    }
    Put(out, size, "Always verify", "%s", f.always_uv ? "on" : "off");
    Put(out, size, "Store", "%u of %u KB%s", static_cast<unsigned>(f.store_used / 1024),
        static_cast<unsigned>(f.store_size / 1024), f.encrypted ? ", encrypted" : "");
    Put(out, size, "Signatures", "%u", static_cast<unsigned>(f.sign_count));
    Put(out, size, "Presence", "%s",
        config::Get().test.auto_presence ? "AUTO (config test)" : "Left / Home yes, Right no");
}

// Settings > Server: registered, connected, the clock against the server's,
// the last sync, the code; a ping (/api/device/ping) at most every
// kServerCheckUs while shown.
constexpr int64_t kServerCheckUs = 60 * 1000000LL;
int64_t server_checked_at = -kServerCheckUs;

// Why a request cannot go now, or nullptr.
const char *ServerBlocked() {
    if (config::Get().server.host[0] == '\0') {
        return "no server set (config.json server.host)";
    }
    if (time(nullptr) < 1704067200) {  // 2024-01-01: TLS checks the dates
        return "the clock is not set";
    }
    if (wifimgr::Get().state != wifimgr::State::kOnline) {
        return "no network: connect Wi-Fi first";
    }
    return nullptr;
}

void DescribeServer(bool summary, char *out, size_t size) {
    const server::Status s = server::Get();
    if (summary) {
        snprintf(out, size, "%s", s.busy ? "connecting..." : server::Summary(s.registered, s.link));
        return;
    }
    const config::Server &c = config::Get().server;
    const int64_t now = time(nullptr);
    if (!s.busy && ServerBlocked() == nullptr &&
        esp_timer_get_time() - server_checked_at >= kServerCheckUs) {
        server_checked_at = esp_timer_get_time();
        server::StartCheck(c.host);
    }
    // Why not: the "Last" line.
    Put(out, size, "State", "%s", s.registered ? "registered" : "not registered");
    Put(out, size, "Connected", "%s",
        s.busy                            ? "asking..."
        : s.link == server::Link::kOk     ? "yes"
        : s.link == server::Link::kFailed ? "no"
                                          : "not tried yet");
    char text[40];  // OffsetText: "-432000 s (5 days), critical"
    if (s.has_offset) {
        server::OffsetText(s.offset, text, sizeof text);
        Put(out, size, "Clock", "%s", text);
    }
    if (s.registered) {
        Put(out, size, "Signed in", "%s",
            s.token_left > 0      ? "yes"
            : s.has_client_id     ? "not yet"
                                  : "no: register again");
    }
    server::Ago(now, s.last_sync, text, sizeof text);
    Put(out, size, "Status sent", "%s", text);
    uint32_t code = 0;
    if (s.registered && server::Code(now, &code)) {
        char shown[8];
        server::CodeText(code, shown);
        Put(out, size, "Code", "%s", shown);
    }
    Put(out, size, "Server", "%s", c.host[0] != '\0' ? c.host : "none");
    Put(out, size, "E-mail", "%s", c.email[0] != '\0' ? c.email : "not set");
    if (s.said[0] != '\0') {
        Put(out, size, "Last", "%s", s.said);
    }
}

// Server > Sync: each store against the server's copy.
void DescribeSync(char *out, size_t size) {
    namespace sy = server::sync;
    const server::Status s = server::Get();
    for (const sy::Store store : sy::kAll) {
        const server::StoreStatus &t = s.stores[static_cast<size_t>(store)];
        const char *name = store == sy::Store::kVault ? "Passwords" : "Passkeys";
        if (sy::ByHand(t.phase) || t.phase == sy::Phase::kNewer) {
            // "conflict, v318 / v319": here / the server's.
            Put(out, size, name, "%s, v%u / v%u",
                t.phase == sy::Phase::kNeedPin ? "its PIN" : sy::PhaseText(t.phase),
                static_cast<unsigned>(t.local_version), static_cast<unsigned>(t.server_version));
            if (t.other_store) {
                Put(out, size, "  server's", "another store");
            }
            if (t.server_at[0] != '\0') {
                Put(out, size, "  sent", "%s UTC", t.server_at);
            }
        } else if (t.local_version != 0) {
            Put(out, size, name, "%s (v%u)", sy::PhaseText(t.phase),
                static_cast<unsigned>(t.local_version));
        } else {
            Put(out, size, name, "%s", t.phase == sy::Phase::kNone ? "not checked yet"
                                                                    : sy::PhaseText(t.phase));
        }
    }
    char text[40];
    server::Ago(time(nullptr), s.synced_at, text, sizeof text);
    Put(out, size, "Last sync", "%s", text);
    const uint8_t minutes = config::Get().server.sync_minutes;
    if (minutes == 0) {
        Put(out, size, "Automatic", "never (only Sync now)");
    } else {
        Put(out, size, "Automatic", "on changes, every %u min", static_cast<unsigned>(minutes));
    }
    if (s.sync_said[0] != '\0') {
        Put(out, size, "Last", "%s", s.sync_said);
    }
    // The account's stores under other seed phrases: they never open here.
    if (!s.others_known) {
        return;
    }
    if (s.others.count == 0) {
        Put(out, size, "Other phrases", "none");
        return;
    }
    const sy::Other &o = s.others.shown;
    Put(out, size, "Other phrases", "%u store%s", static_cast<unsigned>(s.others.count),
        s.others.count == 1 ? "" : "s");
    Put(out, size, "  seed", "%02x%02x%02x%02x: %s v%u", o.seed_id[0], o.seed_id[1], o.seed_id[2],
        o.seed_id[3], sy::KindName(o.kind), static_cast<unsigned>(o.version));
    if (o.at[0] != '\0') {
        Put(out, size, "  sent", "%s UTC", o.at);
    }
    Put(out, size, "  what", "%s",
        o.why == sy::OtherKind::kMoved   ? "ours, moved: type that phrase"
        : o.why == sy::OtherKind::kOther ? "another device's phrase"
                                         : "ours, an old phrase");
}

void SyncSummary(char *out, size_t size) {
    if (config::Get().server.host[0] == '\0') {
        snprintf(out, size, "no server");
        return;
    }
    const server::Status s = server::Get();
    switch (s.icon) {
        case server::sync::Icon::kHidden:
        case server::sync::Icon::kOff: snprintf(out, size, "not registered"); return;
        case server::sync::Icon::kPending: snprintf(out, size, "changes to send"); return;
        case server::sync::Icon::kOk: snprintf(out, size, "in sync"); return;
        case server::sync::Icon::kConflict: snprintf(out, size, "waits for you"); return;
    }
}

uint8_t SyncWaits() {
    const server::Status s = server::Get();
    uint8_t bits = 0;
    for (size_t i = 0; i < server::sync::kStores; ++i) {
        const server::sync::Phase p = s.stores[i].phase;
        bits |= static_cast<uint8_t>((p == server::sync::Phase::kConflict ? 1u : 0u) << (2 * i));
        bits |= static_cast<uint8_t>((p == server::sync::Phase::kNeedPin ? 1u : 0u) << (2 * i + 1));
    }
    return bits;
}

const char *SyncResolve(uint8_t store, bool take, const char *pin) {
    if (const char *why = ServerBlocked()) {
        return why;
    }
    switch (server::StartResolve(static_cast<server::sync::Store>(store),
                                 take ? server::Choice::kTakeServer : server::Choice::kKeepMine,
                                 pin)) {
        case ESP_OK: return take ? "taking the server's copy..." : "sending this device's copy...";
        case ESP_ERR_NOT_FOUND: return "not registered";
        case ESP_ERR_INVALID_STATE: return "busy, or nothing waits";
        default: return "cannot resolve";
    }
}

// --- Server > Firmware (ui::ServerHooks) ---------------------------------------------

// The releases as the server listed them (ui task: off its stack).
EXT_RAM_BSS_ATTR server::firmware::Release firmware_list[server::firmware::kMaxReleases];

uint8_t FirmwareRows(ui::settings::FirmwareRow *out, uint8_t max) {
    const char *running = esp_app_get_description()->version;
    const size_t n = server::Releases(
        firmware_list, max < server::firmware::kMaxReleases ? max : server::firmware::kMaxReleases);
    for (size_t i = 0; i < n; ++i) {
        const server::firmware::Release &r = firmware_list[i];
        ui::settings::FirmwareRow &row = out[i];
        snprintf(row.id, sizeof row.id, "%s", r.id);
        snprintf(row.version, sizeof row.version, "%s", r.version);
        snprintf(row.built, sizeof row.built, "%s", r.built);
        row.size = r.size;
        row.active = r.active;
        row.running = strcmp(r.version, running) == 0;
        row.newer = server::firmware::Newer(r.version, running);
    }
    return static_cast<uint8_t>(n);
}

void FirmwareSummary(char *out, size_t size) {
    const server::Status s = server::Get();
    if (config::Get().server.host[0] == '\0') {
        snprintf(out, size, "no server");
    } else if (!s.registered) {
        snprintf(out, size, "not registered");
    } else if (!s.firmware_known) {
        snprintf(out, size, "not checked yet");
    } else if (s.firmware_newer && server::Releases(firmware_list, 1) == 1) {
        snprintf(out, size, "new: %s", firmware_list[0].version);
    } else {
        snprintf(out, size, "%s", s.firmware_count == 0 ? "none on the server" : "up to date");
    }
}

// "label\tvalue" lines over the list.
void FirmwareNote(char *out, size_t size) {
    out[0] = '\0';
    const server::Status s = server::Get();
    Put(out, size, "Running", "%s", esp_app_get_description()->version);
    char text[40];
    server::Ago(time(nullptr), s.firmware_at, text, sizeof text);
    Put(out, size, "Checked", "%s", text);
    if (s.firmware_said[0] != '\0') {
        Put(out, size, "Last", "%s", s.firmware_said);
    }
}

// What the page shows, for its redraw: the lines and the releases.
void DescribeFirmware(char *out, size_t size) {
    FirmwareNote(out, size);
    static EXT_RAM_BSS_ATTR ui::settings::FirmwareRow rows[ui::settings::kFirmwareRows];
    const uint8_t n = FirmwareRows(rows, ui::settings::kFirmwareRows);
    for (uint8_t i = 0; i < n; ++i) {
        Put(out, size, rows[i].version, "%s", ui::settings::FirmwareTag(rows[i]));
    }
}

const char *FirmwareInstall(const char *id) {
    if (const char *why = ServerBlocked()) {
        return why;
    }
    switch (server::StartInstall(id)) {
        case ESP_OK: return "downloading...";
        case ESP_ERR_NOT_FOUND: return "not in the server's list: check again";
        case ESP_ERR_INVALID_STATE: {
            const char *why = ota::Refusal();
            return why != nullptr ? why : "busy: a request is under way";
        }
        default: return "cannot install";
    }
}

uint8_t ServerSyncMinutes() { return config::Get().server.sync_minutes; }

const char *ServerSetSyncMinutes(uint8_t minutes) {
    if (!config::ValidStatusMinutes(minutes)) {
        return "not a choice";
    }
    config::Get().server.sync_minutes = minutes;
    return config::Save() == ESP_OK ? "saved" : "in use, but config.json is not saved";
}

// --- The stores for the sync (server::Hooks, the server's task) ---------------------

const char *SyncPath(server::sync::Store store) {
    if (store == server::sync::Store::kVault) {
        const vault::State st = vault::GetState();
        return st == vault::State::kLocked || st == vault::State::kOpen ? vault::StoreFile().path
                                                                        : nullptr;
    }
    // Refused (rollback, damaged): left alone. Locked: the file goes up as it is.
    if (fido_file == nullptr ||
        (vault::GetState() == vault::State::kOpen && fido_ctap_stopped_reason() != nullptr)) {
        return nullptr;
    }
    return fido_file;
}

server::Taken SyncTake(server::sync::Store store, const char *from,
                       const uint8_t store_id[server::sync::kStoreIdSize], bool force,
                       const char *pin, char *said, size_t size) {
    const storefile::File fido = {fido_file, storefile::Kind::kFido, "fido"};
    const storefile::File &target =
        store == server::sync::Store::kVault ? vault::StoreFile() : fido;
    if (target.path == nullptr) {
        snprintf(said, size, "no store file");
        return server::Taken::kFailed;
    }
    const vault::Error e = vault::Adopt(target, from, store_id, force, pin);
    switch (e) {
        case vault::Error::kOk: return server::Taken::kOk;
        case vault::Error::kLocked: return server::Taken::kLocked;
        case vault::Error::kNeedPin: return server::Taken::kNeedPin;
        case vault::Error::kWrongPin:
            snprintf(said, size, "wrong PIN for the server's copy");
            return server::Taken::kWrongPin;
        case vault::Error::kBadCopy:
            snprintf(said, size, "%s", vault::Describe(e));
            return server::Taken::kBad;
        default:
            snprintf(said, size, "%s", vault::Describe(e));
            return server::Taken::kFailed;
    }
}

void ServerEmail(char *out, size_t size) { snprintf(out, size, "%s", config::Get().server.email); }

const char *ServerRegister(const char *email) {
    if (!config::ValidEmail(email) || email[0] == '\0') {
        return "not an e-mail address";
    }
    if (const char *why = ServerBlocked()) {
        return why;
    }
    const server::Status s = server::Get();
    if (s.has_offset && server::ClockTooFar(s.offset)) {
        return "the clock is over 10 min off the server's";
    }
    config::Server &c = config::Get().server;
    snprintf(c.email, sizeof c.email, "%s", email);
    const bool saved = config::Save() == ESP_OK;
    switch (server::StartRegister(c.host, c.email)) {
        case ESP_OK: return saved ? "registering..." : "registering (config.json not saved)";
        case ESP_ERR_INVALID_STATE: return "busy: a request is under way";
        default: return "cannot register";
    }
}

uint8_t ServerStatusMinutes() { return config::Get().server.status_minutes; }

const char *ServerSetStatusMinutes(uint8_t minutes) {
    if (!config::ValidStatusMinutes(minutes)) {
        return "not a choice";
    }
    config::Get().server.status_minutes = minutes;
    return config::Save() == ESP_OK ? "saved" : "in use, but config.json is not saved";
}

const ui::ServerHooks kServerHooks = {ServerEmail,       ServerRegister,       ServerStatusMinutes,
                                      ServerSetStatusMinutes, ServerSyncMinutes, ServerSetSyncMinutes,
                                      SyncSummary,       SyncWaits,            SyncResolve,
                                      FirmwareRows,      FirmwareSummary,      FirmwareNote,
                                      FirmwareInstall};

// The status schedule (server::Hooks): from the scheduler's task.
const server::Hooks kServerSchedule = {
    .ready = [] { return ServerBlocked() == nullptr; },
    .status_minutes = ServerStatusMinutes,
    .host = [](char *out, size_t size) { snprintf(out, size, "%s", config::Get().server.host); },
    .report =
        [](server::Report *r) {
            const time_t now = time(nullptr);
            r->firmware = esp_app_get_description()->version;
            r->board = boards::kBoardId == boards::BoardId::kX4Pro ? "x4pro" : "dev";
            r->wifi = wifimgr::Get().state != wifimgr::State::kOff;
            r->ble = ble::IsOn();
            r->battery = battery::Level();
            r->uptime = esp_timer_get_time() / 1000000;
            r->time = now >= 1704067200 ? static_cast<int64_t>(now) : 0;
        },
    .sync_minutes = ServerSyncMinutes,
    .store_path = SyncPath,
    .store_version =
        [](server::sync::Store store) {
            return store == server::sync::Store::kVault ? vault::Version()
                                                        : fido_ctap_file_version();
        },
    .open = [] { return vault::GetState() == vault::State::kOpen; },
    .empty =
        [](server::sync::Store store) {
            if (store == server::sync::Store::kVault) {
                return vault::GetInfo().count == 0;
            }
            fido_ctap_info_t info = {};
            return fido_ctap_info(&info) == ESP_OK && info.credentials == 0;
        },
    .rewrite =
        [](server::sync::Store store, uint32_t min_version) {
            if (store == server::sync::Store::kVault) {
                return vault::Rewrite(min_version) == vault::Error::kOk ? ESP_OK : ESP_FAIL;
            }
            return fido_ctap_rewrite(min_version);
        },
    .take = SyncTake,
};

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
        case Page::kServer: DescribeServer(summary, out, size); break;
        case Page::kTouch:
            if (summary) {
                const config::TouchCalibration &t = config::Get().display.touch;
                const bool calibrated = t.x_scale != 1000 || t.x_offset != 0 ||
                                        t.y_scale != 1000 || t.y_offset != 0;
                snprintf(out, size, "%s", calibrated ? "calibrated" : "as it comes");
            }
            break;
        case Page::kMain:
        case Page::kWifiScan:
        case Page::kWifiPassword:
        case Page::kBlePair:
        case Page::kDisplayChoice:
        case Page::kTimeSync:
        case Page::kTimeRegion:
        case Page::kTimeZone:
        case Page::kTimeSet:
        case Page::kSecurityLock:
        case Page::kSecurityPin:
        case Page::kWebMode:
        case Page::kWebLogin:
        case Page::kBleName:
        case Page::kWifiMore:
        case Page::kWifiChoice:
        case Page::kWifiText:
        case Page::kWifiStatic:
        case Page::kWifiStaticText:
        case Page::kWifiHidden:
        case Page::kServerEmail:
        case Page::kServerConfig:
        case Page::kServerSyncPin: break;
        case Page::kServerSync:
            if (!summary) {
                DescribeSync(out, size);
            }
            break;
        case Page::kServerFirmware:
            if (!summary) {
                DescribeFirmware(out, size);
            }
            break;
        case Page::kFido: DescribeFido(summary, out, size); break;
        case Page::kWeb:
            if (summary) {
                const web::Status w = web::Get();
                const wifimgr::Snapshot net = wifimgr::Get();
                if (w.running && net.radio.ip != 0) {
                    char ip[16];
                    Ip(net.radio.ip, ip, sizeof ip);
                    snprintf(out, size, "%s", ip);
                } else {
                    snprintf(out, size, "%s", w.running ? "on" : "off");
                }
            }
            break;
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
        case Action::kRestart:
            esp_restart();
            break;
        case Action::kFidoReset:
            switch (fido_ctap_reset()) {
                case ESP_OK: return "FIDO2 reset: no keys, no PIN";
                case ESP_ERR_TIMEOUT: return "busy: a FIDO2 request is running";
                default: return "cannot reset FIDO2";
            }
        case Action::kServerStatus:
            if (const char *why = ServerBlocked()) {
                return why;
            }
            switch (server::StartStatus()) {
                case ESP_OK: return "sending the status";
                case ESP_ERR_NOT_FOUND: return "not registered";
                case ESP_ERR_INVALID_STATE: return "busy: a request is under way";
                default: return "cannot send the status";
            }
        case Action::kServerSync:
            if (const char *why = ServerBlocked()) {
                return why;
            }
            switch (server::StartSync()) {
                case ESP_OK: return "syncing";
                case ESP_ERR_NOT_FOUND: return "not registered";
                case ESP_ERR_INVALID_STATE: return "busy: a request is under way";
                default: return "cannot sync";
            }
        case Action::kServerFirmware:
            if (const char *why = ServerBlocked()) {
                return why;
            }
            switch (server::StartFirmwareList()) {
                case ESP_OK: return "asking the server...";
                case ESP_ERR_NOT_FOUND: return "not registered";
                case ESP_ERR_INVALID_STATE: return "busy: a request is under way";
                default: return "cannot ask the server";
            }
        case Action::kServerRegister:  // the ui asks the e-mail (ServerRegister)
        case Action::kNone: break;
    }
    return "";
}

// --- The Wi-Fi window (ui::WifiHooks) -------------------------------------------------

void WifiView(ui::wifi_view::View *out) {
    namespace wv = ui::wifi_view;
    *out = {};
    const wifimgr::Snapshot w = wifimgr::Get();
    const config::Wifi &cfg = config::Get().wifi;
    out->active = cfg.active;
    const bool online = w.state == wifimgr::State::kOnline;
    const bool ap = w.state == wifimgr::State::kAp || w.state == wifimgr::State::kApWindow ||
                    w.state == wifimgr::State::kIdle;
    const char *state = w.state == wifimgr::State::kOff ? "Off"
                        : online ? (w.internet == wifimgr::Internet::kOffline ? "No internet"
                                                                              : "Online")
                        : ap     ? "Access point"
                                 : "Connecting...";
    snprintf(out->state, sizeof out->state, "%s", state);
    if (online) {
        Ip(w.radio.ip, out->detail, sizeof out->detail);
    } else if (ap) {
        char ip[16];
        Ip(w.radio.ap_ip, ip, sizeof ip);
        snprintf(out->detail, sizeof out->detail, "%s %s", w.radio.ap_ssid, ip);
    }
    out->joined = wv::kNone;
    for (uint8_t i = 0; i < cfg.network_count && i < wv::kMaxSaved; ++i) {
        snprintf(out->saved[i].ssid, sizeof out->saved[i].ssid, "%s", cfg.networks[i].ssid);
        out->saved[i].refused = w.auth_failed[i];
        if (online && w.network == i) {
            out->joined = static_cast<int8_t>(i);
        }
    }
    out->saved_count = cfg.network_count < wv::kMaxSaved ? cfg.network_count : wv::kMaxSaved;
    out->mode = static_cast<uint8_t>(cfg.mode);
    snprintf(out->ap_name, sizeof out->ap_name, "%s", cfg.ap_ssid);
    out->ap_password = cfg.ap_password[0] != '\0';
    out->channel = cfg.ap_channel;
    out->rounds = cfg.rounds;
    out->window = cfg.ap_window_seconds;
    snprintf(out->hostname, sizeof out->hostname, "%s", cfg.hostname);
}

size_t WifiScan(ui::wifi_view::Nearby *out, size_t capacity) {
    static EXT_RAM_BSS_ATTR wifi::ScanResult found[wifi::kMaxScanResults];  // off the caller's stack
    size_t count = 0;
    if (wifimgr::Scan(found, wifi::kMaxScanResults, &count) != ESP_OK) {
        return 0;
    }
    const size_t n = count < capacity ? count : capacity;
    for (size_t i = 0; i < n; ++i) {
        snprintf(out[i].ssid, sizeof out[i].ssid, "%s", found[i].ssid);
        out[i].rssi = found[i].rssi;
        out[i].secured = found[i].secured;
    }
    return n;
}

// Saved, then handed to the manager (as the console's `wifi`).
const char *WifiSave(const char *done) {
    wifimgr::Apply();
    return config::Save() == ESP_OK ? done : "applied, but config.json is not saved";
}

const char *WifiSetActive(bool on) {
    config::Get().wifi.active = on;
    return WifiSave(on ? "Wi-Fi on" : "Wi-Fi off");
}

const char *WifiJoin(const char *ssid, const char *password) {
    config::Wifi &w = config::Get().wifi;
    if (password != nullptr) {
        char error[config::kErrorSize] = {};
        if (!config::SetNetwork(&w, ssid, password, error, sizeof error)) {
            return strstr(error, "full") != nullptr ? "4 networks saved: forget one first"
                                                    : "the password does not fit (8..63)";
        }
    }
    // Tried first: moved to the top of the list.
    uint8_t at = 0;
    for (; at < w.network_count && strcmp(w.networks[at].ssid, ssid) != 0; ++at) {
    }
    if (at == w.network_count) {
        return "no such saved network";
    }
    static config::Network moved;  // ~200 bytes: off the stack
    moved = w.networks[at];
    for (uint8_t i = at; i > 0; --i) {
        w.networks[i] = w.networks[i - 1];
    }
    w.networks[0] = moved;
    memset(&moved, 0, sizeof moved);
    w.active = true;
    return WifiSave("connecting...");
}

const char *WifiForget(const char *ssid) {
    if (!config::RemoveNetwork(&config::Get().wifi, ssid)) {
        return "no such saved network";
    }
    return WifiSave("forgotten");
}

// --- The Bluetooth window (ui::BleHooks) ---------------------------------------------

void BleView(ui::ble_view::View *out) {
    namespace bv = ui::ble_view;
    *out = {};
    out->active = config::Get().ble.active;
    out->on = ble::IsOn();
    const bool connected = out->on && ble::ConnHandle() != BLE_HS_CONN_HANDLE_NONE;
    const char *state = !out->on                         ? "Off"
                        : ble::PairingSecondsLeft() > 0 ? "Pairing"
                        : connected                     ? "Connected"
                                                        : "Waiting for a host";
    snprintf(out->state, sizeof out->state, "%s", state);
    snprintf(out->name, sizeof out->name, "%s", ble::Name());
    static ble::BondInfo bonds[bv::kMaxHosts];  // off the ui task's stack
    const size_t n = ble::Bonds(bonds, bv::kMaxHosts);
    for (size_t i = 0; i < n; ++i) {
        snprintf(out->hosts[i].addr, sizeof out->hosts[i].addr, "%s", bonds[i].addr);
        out->hosts[i].connected = bonds[i].connected;
        out->hosts[i].target = bonds[i].target;
    }
    out->host_count = static_cast<uint8_t>(n);
}

bool BlePairingEvent(ui::ble_view::Pairing *what, uint32_t *passkey) {
    ble::PairingEvent e = {};
    if (!ble::WaitPairingEvent(&e, 0)) {
        return false;
    }
    *what = e.type == ble::PairingEvent::kPasskey ? ui::ble_view::Pairing::kPasskey
            : e.type == ble::PairingEvent::kPaired ? ui::ble_view::Pairing::kPaired
                                                   : ui::ble_view::Pairing::kFailed;
    *passkey = e.passkey;
    return true;
}

uint32_t BlePairingLeft() { return ble::PairingSecondsLeft(); }

// On or off now, and so at boot (as the console's `ble on` and `ble boot`).
const char *BleSetActive(bool on) {
    const esp_err_t err = on ? ble::On() : ble::Off();
    config::Get().ble.active = on;
    if (config::Save() != ESP_OK) {
        return "config.json is not saved";
    }
    return err != ESP_OK ? "cannot switch Bluetooth" : on ? "Bluetooth on" : "Bluetooth off";
}

const char *BleUse(const char *addr) {
    return ble::UseBond(addr) != ESP_OK ? "cannot"
           : addr == nullptr          ? "any paired host may connect"
                                      : "only this host may connect";
}

const char *BleForget(const char *addr) {
    return ble::Unpair(addr) == ESP_OK ? "forgotten" : "cannot forget it";
}

const char *BlePair(bool start) {
    if (!start) {
        ble::StopPairing();
        return "pairing closed";
    }
    return ble::StartPairing(ui::ble_view::kPairSeconds) == ESP_OK ? "pairing open"
                                                                 : "cannot open pairing";
}

// The advertised name, applied at once (advertising restarts) and saved
// (as config.json `ble.name`; "": the board's).
const char *BleSetName(const char *name) {
    config::Ble &cfg = config::Get().ble;
    if (name != nullptr && !config::ValidBleName(name)) {
        return "1..29 characters";
    }
    snprintf(cfg.name, sizeof cfg.name, "%s", name != nullptr ? name : boards::kBoard.ble_name);
    ble::SetName(cfg.name);
    return config::Save() == ESP_OK ? "name saved" : "in use, but config.json is not saved";
}

const ui::BleHooks kBleHooks = {BleView, BlePairingEvent, BlePairingLeft, BleSetActive,
                                BleUse,  BleForget,       BlePair,        BleSetName};

// --- The Devices window (ui::DisplayHooks) -------------------------------------------

void DisplayView(ui::display_view::View *out) {
    namespace dv = ui::display_view;
    // The gauge over I2C: read every 30 s, as for the status bar.
    constexpr int64_t kBatteryMs = 30 * 1000;
    static int64_t read_at = -kBatteryMs;
    static int level = -1;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - read_at >= kBatteryMs) {
        level = battery::Level();
        read_at = now_ms;
    }
    out->battery = static_cast<int8_t>(level);
    out->charging = battery::ChargeState() == battery::Charge::kCharging;
    const config::Data &c = config::Get();
    for (uint8_t i = 0; i < dv::kSettingCount; ++i) {
        switch (dv::kSettings[i]) {
            case dv::Setting::kIdle: out->value[i] = c.power.idle_minutes; break;
            case dv::Setting::kIdleUsb: out->value[i] = c.power.idle_minutes_usb; break;
            case dv::Setting::kLightOff: out->value[i] = c.frontlight.off_minutes; break;
            case dv::Setting::kLightOffUsb: out->value[i] = c.frontlight.off_minutes_usb; break;
            case dv::Setting::kKeepPin: out->value[i] = c.power.keep_pin_hours; break;
            case dv::Setting::kWallpaper:
                out->value[i] = c.display.wallpaper == config::WallpaperMode::kBw ? 1 : 0;
                break;
            case dv::Setting::kHoldRail: out->value[i] = c.power.hold_rail ? 1 : 0; break;
            case dv::Setting::kUsb: out->value[i] = static_cast<uint16_t>(usb::GetMode()); break;
            case dv::Setting::kFastRefreshes: out->value[i] = c.display.fast_refreshes; break;
            case dv::Setting::kFullFrom: out->value[i] = c.display.full_from_percent; break;
        }
    }
}

// Read where used (power, the sleep, the wallpaper): saved is applied.
const char *DisplaySet(ui::display_view::Setting setting, uint16_t value) {
    namespace dv = ui::display_view;
    config::Data &c = config::Get();
    switch (setting) {
        case dv::Setting::kIdle: c.power.idle_minutes = value; break;
        case dv::Setting::kIdleUsb: c.power.idle_minutes_usb = value; break;
        case dv::Setting::kLightOff: c.frontlight.off_minutes = value; break;
        case dv::Setting::kLightOffUsb: c.frontlight.off_minutes_usb = value; break;
        case dv::Setting::kKeepPin: c.power.keep_pin_hours = static_cast<uint8_t>(value); break;
        case dv::Setting::kWallpaper:
            c.display.wallpaper = value == 1 ? config::WallpaperMode::kBw : config::WallpaperMode::kGrey;
            break;
        case dv::Setting::kHoldRail: c.power.hold_rail = value == 1; break;
        case dv::Setting::kFastRefreshes:
            c.display.fast_refreshes = value < config::kMaxFastRefreshes ? value : config::kMaxFastRefreshes;
            break;
        case dv::Setting::kFullFrom:
            c.display.full_from_percent = static_cast<uint8_t>(value < 100 ? value : 100);
            break;
        case dv::Setting::kUsb: {
            if (value == static_cast<uint16_t>(usb::Mode::kReader)) {  // for a while: not saved
                const esp_err_t err = usb::SetMode(usb::Mode::kReader);
                return err == ESP_OK              ? "the card is a USB drive"
                       : err == ESP_ERR_NOT_FOUND ? "no microSD card"
                                                  : "the card reader did not start";
            }
            if (value > static_cast<uint16_t>(config::UsbMode::kSerialFido)) {
                return "no such USB mode";
            }
            c.usb.mode = static_cast<config::UsbMode>(value);
            const bool saved = config::Save() == ESP_OK;
            if (usb::SetMode(static_cast<usb::Mode>(value)) != ESP_OK) {
                return "the port did not start: Serial/JTAG";
            }
            return saved ? "saved" : "in use, but config.json is not saved";
        }
    }
    return config::Save() == ESP_OK ? "saved" : "in use, but config.json is not saved";
}

const ui::DisplayHooks kDisplayHooks = {DisplayView, DisplaySet};

// The e-ink refresh plan, read before each refresh: changes apply at once.
void RefreshSource(uint16_t *fast, uint8_t *full_from_percent) {
    const config::Display &d = config::Get().display;
    *fast = d.fast_refreshes;
    *full_from_percent = d.full_from_percent;
}

// --- The Date & time window (ui::TimeHooks) ------------------------------------------

void TimeView(ui::time_view::View *out) {
    namespace tv = ui::time_view;
    *out = {};
    const time_t now = time(nullptr);
    out->time_set = rtc::Believable(now);
    struct tm local = {};
    localtime_r(&now, &local);
    strftime(out->clock, sizeof out->clock, "%H:%M", &local);
    strftime(out->date, sizeof out->date, "%A, %d %B %Y", &local);
    out->now[tv::kYear] = static_cast<uint16_t>(local.tm_year + 1900);
    out->now[tv::kMonth] = static_cast<uint16_t>(local.tm_mon + 1);
    out->now[tv::kDay] = static_cast<uint16_t>(local.tm_mday);
    out->now[tv::kHour] = static_cast<uint16_t>(local.tm_hour);
    out->now[tv::kMinute] = static_cast<uint16_t>(local.tm_min);
    const config::Time &t = config::Get().time;
    snprintf(out->zone, sizeof out->zone, "%s", t.zone[0] != '\0' ? t.zone : "UTC");
    const int offset = tz::OffsetSeconds();
    const int a = offset < 0 ? -offset : offset;
    snprintf(out->offset, sizeof out->offset, "UTC%c%02d:%02d%s", offset < 0 ? '-' : '+', a / 3600,
             a / 60 % 60, tz::IsDaylightSaving() ? ", summer" : "");
    snprintf(out->server, sizeof out->server, "%s", t.server);
    out->sync_hours = t.sync_hours;
    const timesync::Status s = timesync::Get();
    if (s.syncing) {
        snprintf(out->last_sync, sizeof out->last_sync, "now");
    } else if (!s.ever_synced) {
        snprintf(out->last_sync, sizeof out->last_sync, "never");
    } else if (s.since_last_ms < 3600u * 1000) {
        snprintf(out->last_sync, sizeof out->last_sync, "%u min ago",
                 static_cast<unsigned>(s.since_last_ms / 60000));
    } else {
        snprintf(out->last_sync, sizeof out->last_sync, "%u h ago",
                 static_cast<unsigned>(s.since_last_ms / 3600000));
    }
}

size_t ZoneCount() { return tz::Count(); }

const char *ZoneName(size_t index) { return tz::At(index).name; }

// As the console's `date zone`.
const char *TimeSetZone(const char *name) {
    config::Time &t = config::Get().time;
    if (tz::ApplyZone(name) != ESP_OK) {
        return "not a zone";
    }
    snprintf(t.zone, sizeof t.zone, "%s", name);
    return config::Save() == ESP_OK ? "zone set" : "in use, but config.json is not saved";
}

// As the console's `date hours`.
const char *TimeSetSync(uint8_t hours) {
    config::Get().time.sync_hours = hours;
    timesync::Apply();
    return config::Save() == ESP_OK ? "saved" : "in use, but config.json is not saved";
}

// As the console's `date set` (local time): the system clock, then the board's.
const char *TimeSetClock(const ui::time_view::Manual &m) {
    namespace tv = ui::time_view;
    struct tm local = {};
    local.tm_year = m.value[tv::kYear] - 1900;
    local.tm_mon = m.value[tv::kMonth] - 1;
    local.tm_mday = m.value[tv::kDay];
    local.tm_hour = m.value[tv::kHour];
    local.tm_min = m.value[tv::kMinute];
    local.tm_isdst = -1;  // libc decides from the zone
    const time_t seconds = mktime(&local);
    if (!rtc::Believable(seconds)) {
        return "not a believable time";
    }
    const timeval set = {.tv_sec = seconds, .tv_usec = 0};
    settimeofday(&set, nullptr);
    return rtc::StoreSystemTime(boards::Clock()) == ESP_OK ? "the time is set"
                                                            : "set, but not on the board's clock";
}

const ui::TimeHooks kTimeHooks = {TimeView,    ZoneCount,   ZoneName,
                                  TimeSetZone, TimeSetSync, TimeSetClock};

// --- The Security window (ui::SecurityHooks) -----------------------------------------

void SecurityView(ui::security_view::View *out) {
    *out = {};
    const vault::Info v = vault::GetInfo();
    out->open = v.state == vault::State::kOpen;
    out->records = static_cast<uint16_t>(v.count);
    out->wrong = v.pin_failures;
    out->max_wrong = vault::kMaxPinTries;
    fido_ctap_info_t f = {};
    // Not while a request waits for the button (the store's lock is taken).
    if (!fido_waiting && fido_ctap_info(&f) == ESP_OK) {
        out->fido_known = true;
        out->fido_keys = static_cast<uint16_t>(f.credentials);
        out->fido_max = static_cast<uint16_t>(f.max_credentials);
        out->fido_pin = f.pin_set;
    }
    const config::Pin &pin = config::Get().pin;
    out->lock_minutes = pin.lock_minutes;
    out->wipe = pin.wipe[0] != '\0';
    out->complexity = pin.complexity;
}

// The vault takes the policy at once (as the console's `pin wipe`).
const char *SecuritySave(const char *done) {
    const config::Pin &cfg = config::Get().pin;
    vault::SetPolicy({static_cast<uint32_t>(cfg.lock_minutes) * 60, cfg.wipe, cfg.complexity});
    return config::Save() == ESP_OK ? done : "in use, but config.json is not saved";
}

// As the console's `pin change`: vault.bin and fido.bin re-keyed.
const char *SecurityChangePin(const char *old_pin, const char *new_pin) {
    static char text[48];
    const vault::Error e = vault::ChangePin(old_pin, new_pin);
    switch (e) {
        case vault::Error::kOk: return "PIN changed";
        case vault::Error::kWeakPin: return "too simple: choose another PIN";
        case vault::Error::kWrongPin: {
            const vault::Info info = vault::GetInfo();
            snprintf(text, sizeof text, "wrong PIN, %u tries left",
                     static_cast<unsigned>(info.pin_failures < vault::kMaxPinTries
                                               ? vault::kMaxPinTries - info.pin_failures
                                               : 0));
            return text;
        }
        default: return vault::Describe(e);
    }
}

const char *SecuritySetWipe(const char *pin) {
    config::Pin &cfg = config::Get().pin;
    if (pin == nullptr) {
        cfg.wipe[0] = '\0';
        return SecuritySave("wipe PIN off");
    }
    static char wipe[vault::kWipeTextSize];  // off the task's stack
    const vault::Error e = vault::MakeWipe(pin, wipe);
    if (e != vault::Error::kOk) {
        vault::Wipe(wipe, sizeof wipe);
        return e == vault::Error::kPinInUse ? "that is the PIN: choose another" : vault::Describe(e);
    }
    snprintf(cfg.wipe, sizeof cfg.wipe, "%s", wipe);
    vault::Wipe(wipe, sizeof wipe);
    return SecuritySave("wipe PIN set");
}

const char *SecuritySetLock(uint16_t minutes) {
    config::Get().pin.lock_minutes = minutes;
    return SecuritySave("saved");
}

// The vault at once; FIDO2 reads it at its start (fido_ctap_set_pin_complexity).
const char *SecuritySetComplexity(bool on) {
    config::Get().pin.complexity = on;
    return SecuritySave(on ? "complexity on" : "complexity off");
}

const ui::SecurityHooks kSecurityHooks = {SecurityView, SecurityChangePin, SecuritySetWipe,
                                          SecuritySetLock, SecuritySetComplexity};

// --- The Web site window (ui::WebHooks) ----------------------------------------------

void WebView(ui::web_view::View *out) {
    *out = {};
    const config::Web &cfg = config::Get().web;
    const web::Status w = web::Get();
    const wifimgr::Snapshot net = wifimgr::Get();
    out->running = w.running;
    out->mode = static_cast<uint8_t>(cfg.mode);
    out->write = cfg.write;
    out->login = cfg.user[0] != '\0' && cfg.password[0] != '\0';
    snprintf(out->user, sizeof out->user, "%s", cfg.user);
    char ip[16] = "";
    if (w.running && (net.radio.ip != 0 || net.radio.ap_ip != 0)) {
        Ip(net.radio.ip != 0 ? net.radio.ip : net.radio.ap_ip, ip, sizeof ip);
        snprintf(out->url, sizeof out->url, "http://%s/", ip);
    }
    if (w.running) {
        snprintf(out->state, sizeof out->state, "Running%s", w.auth ? ", with a login" : "");
    } else if (w.desired == web::Desired::kOff) {
        snprintf(out->state, sizeof out->state, "Off");
    } else if (!net.stack_up) {
        snprintf(out->state, sizeof out->state, "Stopped: Wi-Fi is off");
    } else if (w.desired == web::Desired::kAuto) {
        snprintf(out->state, sizeof out->state, "Waits for the access point");
    } else {
        snprintf(out->state, sizeof out->state, "Starting...");
    }
}

// Saved, then the server follows (as the console's `web`).
const char *WebSave(const char *done) {
    web::Apply();
    return config::Save() == ESP_OK ? done : "in use, but config.json is not saved";
}

const char *WebSetMode(uint8_t mode) {
    config::Get().web.mode = static_cast<config::WebMode>(mode);
    return WebSave(mode == 0 ? "web site off" : "saved");
}

const char *WebSetWrite(bool on) {
    config::Get().web.write = on;
    return WebSave(on ? "the site may change settings" : "the site is read-only");
}

// Both or neither: half a credential is an open site.
const char *WebSetLogin(const char *user, const char *password) {
    config::Web &cfg = config::Get().web;
    if (user == nullptr || password == nullptr) {
        cfg.user[0] = '\0';
        vault::Wipe(cfg.password, sizeof cfg.password);
        return WebSave("login off: the site is open");
    }
    if (!config::ValidWebUser(user) || user[0] == '\0' || password[0] == '\0' ||
        strlen(password) >= sizeof cfg.password) {
        return "user: 1..32 characters, no ':'; password: 1..64";
    }
    snprintf(cfg.user, sizeof cfg.user, "%s", user);
    snprintf(cfg.password, sizeof cfg.password, "%s", password);
    return WebSave("login set");
}

const ui::WebHooks kWebHooks = {WebView, WebSetMode, WebSetWrite, WebSetLogin};

// Settings > Touchscreen keeps a new calibration (the ui's own task).
esp_err_t SaveTouch(const ui::touch_cal::Calibration &c) {
    config::Get().display.touch = {c.x_scale, c.x_offset, c.y_scale, c.y_offset};
    return config::Save();
}

bool WifiGetStatic(uint8_t index, ui::wifi_view::StaticIp *out) {
    const config::Wifi &w = config::Get().wifi;
    if (index >= w.network_count) {
        return false;
    }
    const config::StaticIp &ip = w.networks[index].ip;
    out->enabled = ip.enabled;
    snprintf(out->address, sizeof out->address, "%s", ip.address);
    snprintf(out->netmask, sizeof out->netmask, "%s", ip.netmask);
    snprintf(out->gateway, sizeof out->gateway, "%s", ip.gateway);
    snprintf(out->dns, sizeof out->dns, "%s", ip.dns1);
    return true;
}

// As the console's `wifi mode`, `wifi ap`, `wifi set ...`: saved, then applied.
const char *WifiSetChoice(ui::wifi_view::Choice choice, uint16_t value) {
    namespace wv = ui::wifi_view;
    config::Wifi &w = config::Get().wifi;
    switch (choice) {
        case wv::Choice::kMode:
            w.mode = static_cast<config::WifiMode>(value);
            w.active = true;
            break;
        case wv::Choice::kChannel: w.ap_channel = static_cast<uint8_t>(value); break;
        case wv::Choice::kRounds: w.rounds = static_cast<uint8_t>(value); break;
        case wv::Choice::kWindow: w.ap_window_seconds = value; break;
    }
    return WifiSave("saved");
}

const char *WifiSetText(ui::wifi_view::Field field, const char *value) {
    namespace wv = ui::wifi_view;
    config::Wifi &w = config::Get().wifi;
    switch (field) {
        case wv::Field::kApName:
            if (!config::ValidSsid(value)) {
                return "1..32 characters";
            }
            snprintf(w.ap_ssid, sizeof w.ap_ssid, "%s", value);
            break;
        case wv::Field::kApPassword:
            if (!config::ValidApPassword(value)) {
                return "8..63 characters, or empty";
            }
            snprintf(w.ap_password, sizeof w.ap_password, "%s", value);
            break;
        case wv::Field::kHostname:
            if (!wv::HostnameOk(value)) {
                return "1..32 of a-z, 0-9, '-'";
            }
            snprintf(w.hostname, sizeof w.hostname, "%s", value);
            break;
        default: return "not here";
    }
    return WifiSave("saved");
}

// A network's address, checked whole (config::CheckStaticIp) before it is kept.
const char *WifiSetStatic(const char *ssid, const ui::wifi_view::StaticIp &in) {
    config::Wifi &w = config::Get().wifi;
    uint8_t at = 0;
    for (; at < w.network_count && strcmp(w.networks[at].ssid, ssid) != 0; ++at) {
    }
    if (at == w.network_count) {
        return "no such saved network";
    }
    config::StaticIp ip = w.networks[at].ip;  // dns2 kept
    ip.enabled = in.enabled;
    if (in.enabled || in.address[0] != '\0') {  // DHCP: the typed address is kept for later
        snprintf(ip.address, sizeof ip.address, "%s", in.address);
        snprintf(ip.netmask, sizeof ip.netmask, "%s", in.netmask);
        snprintf(ip.gateway, sizeof ip.gateway, "%s", in.gateway);
        snprintf(ip.dns1, sizeof ip.dns1, "%s", in.dns);
    }
    static char error[config::kErrorSize];
    if (!config::CheckStaticIp(ip, error, sizeof error)) {
        return error;
    }
    w.networks[at].ip = ip;
    return WifiSave(in.enabled ? "static address saved" : "DHCP");
}

const ui::WifiHooks kWifiHooks = {WifiView,      WifiScan,      WifiSetActive, WifiJoin,
                                  WifiForget,    WifiGetStatic, WifiSetChoice, WifiSetText,
                                  WifiSetStatic};


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
    ota::Boot();  // a trial boot after an OTA update is confirmed by BootDone's timer
    InitNvs();
    spiffs_fs::Init();  // not fatal: the console reports an unmounted fs
    // config.json from SPIFFS, never the card; built-in defaults if
    // missing/invalid or unmounted.
    config::Init(spiffs_fs::kBasePath);
    // The server's key and TOTP secret (none: not registered).
    server::Init(spiffs_fs::kBasePath);
    // The USB port as config.json has it; stdin / stdout follow it from here.
    static_assert(static_cast<int>(usb::Mode::kSerialFido) ==
                  static_cast<int>(config::UsbMode::kSerialFido));
    usb::Init(static_cast<usb::Mode>(config::Get().usb.mode));
    // The card reader: the card's files may change on the PC, so the vault
    // (and fido.bin) let go of them; the PIN loads them again.
    usb::SetReaderHook([](bool on) {
        if (on) {
            vault::Lock();
        }
        // Finish on the screen: the mode before it back.
        ui::ShowCardReader(on, [] { usb::EndReader(); });
    });
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
    ui::SetWifiHooks(&kWifiHooks);
    ui::SetBleHooks(&kBleHooks);
    ui::SetDisplayHooks(&kDisplayHooks);
    ui::SetRefreshSource(RefreshSource);
    ui::SetTimeHooks(&kTimeHooks);
    ui::SetSecurityHooks(&kSecurityHooks);
    ui::SetWebHooks(&kWebHooks);
    ui::SetServerHooks(&kServerHooks);
    {
        const config::TouchCalibration &t = config::Get().display.touch;
        ui::SetTouchCalibration({t.x_scale, t.x_offset, t.y_scale, t.y_offset}, SaveTouch);
    }

    ble_kb::Register();  // BLE profiles, before the stack starts
    ble_kb::SetTypingHook([] {
        boards::LedTyping();
        power::NoteActivity();
    });
    ble_fido::Register();
    const char *dir = StorageDir();
    if (dir != nullptr) {
        // A PIN or seed change cut short: finished or dropped before anything
        // loads (the seed file with the stores).
        static char marker[64];
        static char vault_path[64];
        static char fido_path[64];
        snprintf(marker, sizeof marker, "%s/%s", dir, vault::kPinChangeMarker);
        snprintf(vault_path, sizeof vault_path, "%s/vault.bin", dir);
        snprintf(fido_path, sizeof fido_path, "%s/fido.bin", dir);
        const char *const paths[] = {vault_path, fido_path, kSeedPath};
        storefile::FinishCommit(marker, paths, 3);
    }
    InitFido(dir);
    InitVault(dir);
    ESP_ERROR_CHECK(console::Init());

    // The web server reconciles on the Wi-Fi manager's tick (hook first); the
    // manager task follows config.json (radio off unless wifi.active).
    web::SetRequestHook(power::NoteActivity);
    ESP_ERROR_CHECK(web::Init());
    ESP_ERROR_CHECK(wifimgr::Init());
    // The status to the server once Wi-Fi is online, then every server.statusMinutes.
    server::StartSchedule(&kServerSchedule);
    // SNTP when the internet is up; the result goes back to the board's clock.
    ESP_ERROR_CHECK(timesync::Init(&boards::Clock()));
    if (config::Get().ble.active) {
        ble::On();
    }
    power::Start();  // the idle watch (power.idleMinutes)
    ota::SetHooks({.activity = [] {
                       boards::LedTyping();
                       power::NoteActivity();
                   },
                   .before_restart = [] {
                       vTaskDelay(pdMS_TO_TICKS(1500));  // "Installed" reaches the glass
                       if (ble::IsOn()) {
                           ble::Off();
                       }
                   },
                   .progress = [](const char *version, const char *source, size_t written,
                                  size_t total) {
                       ui::ShowUpdate(version, source, static_cast<uint32_t>(written),
                                      static_cast<uint32_t>(total));
                   },
                   .finished = ui::EndUpdate});
    boards::BootDone();  // LED: ready
    ota::BootDone();     // everything is up: a trial boot is confirmed in 30 s
}
