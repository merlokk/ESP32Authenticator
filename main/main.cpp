#include <sys/stat.h>

#include <atomic>
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
