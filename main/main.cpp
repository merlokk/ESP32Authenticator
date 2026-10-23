#include <cstdio>
#include <cstring>

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
#include "fido_ctap.h"
#include "nvs_flash.h"
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

// Where store files (fido.bin, vault.bin) live: the SD card when the board has
// one, else SPIFFS. nullptr when that file system is not mounted.
const char *StorageDir() {
    if (boards::kBoard.has_sd_card) {
        ESP_LOGE(TAG, "SD card: no driver yet, no store files");
        return nullptr;
    }
    return spiffs_fs::Mounted() ? spiffs_fs::kBasePath : nullptr;
}

// FIDO2 over BLE: the LionKey CTAP core behind the ble_fido transport. User
// presence: the board's Left button (BOOT on the dev board), debounced; polled
// only by the CTAP request task.
void InitFido(const char *dir) {
    static buttons::Button presence = boards::Button(boards::ButtonId::kLeft);
    presence.Init();
    static char store[64];
    const bool have_fs = dir != nullptr && snprintf(store, sizeof store, "%s/fido.bin", dir) <
                                               static_cast<int>(sizeof store);
    // The only transport today is BLE: the creator of a credential is the host
    // on the link.
    const fido_ctap_hooks_t hooks = {
        ble_fido::Keepalive, ble_fido::Cancelled, [] {
            presence.Poll();
            return presence.Pressed();
        },
        [](fido_ctap_client_t *out) {
            out->transport = FIDO_CTAP_TRANSPORT_BLE;
            return ble::PeerIdentity(&out->addr_type, out->addr);
        },
        [](fido_ctap_wait_t what) {
            boards::LedWaitingForUser(static_cast<boards::UserWait>(what));
        },
        [] { return config::Get().test.auto_presence; }};
    static_assert(static_cast<int>(boards::UserWait::kLongTouchHeld) ==
                  FIDO_CTAP_WAIT_LONG_TOUCH_HELD);
    fido_ctap_set_reset_window(boards::kBoard.fido_reset_window_s);
    fido_ctap_set_pin_complexity(config::Get().pin.complexity);
    if (fido_ctap_init(&hooks, have_fs ? store : nullptr, boards::kBoard.fido_max_credentials) ==
        ESP_OK) {
        ble_fido::SetMessageHandler(fido_ctap_request);
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
    if (vault::Init(dir, boards::kBoard.max_records) != ESP_OK || dir == nullptr) {
        return;
    }
    const char *pin = config::Get().test.pin;
    if (pin[0] != '\0') {
        const vault::Error e = vault::Unlock(pin);
        ESP_LOGW(TAG, "vault: test.pin from config.json: %s", vault::Describe(e));
    }
}

}  // namespace

extern "C" void app_main() {
    boards::Init();  // LED red; the board's clock seeds the system clock (UTC)
    battery::Init();  // not fatal: `battery` reports a missing gauge
    ConfirmRunningApp();
    InitNvs();
    spiffs_fs::Init();  // not fatal: the console reports an unmounted fs
    // config.json from SPIFFS; built-in defaults if missing/invalid or unmounted.
    // TODO: look on the microSD card first.
    config::Init(spiffs_fs::kBasePath);
    tz::ApplyZone(config::Get().time.zone);
    boards::SetLedBrightness(config::Get().led.percent, config::Get().led.idle_percent);

    ui::Init();  // LVGL; no display driver yet

    ble_kb::Register();  // BLE profiles, before the stack starts
    ble_kb::SetTypingHook(boards::LedTyping);
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
    ESP_ERROR_CHECK(web::Init());
    ESP_ERROR_CHECK(wifimgr::Init());
    // SNTP when the internet is up; the result goes back to the board's clock.
    ESP_ERROR_CHECK(timesync::Init(&boards::Clock()));
    if (config::Get().ble.active) {
        ble::On();
    }
    boards::BootDone();  // LED: ready
}
