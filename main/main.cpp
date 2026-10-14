#include <cstdio>

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
// presence: the board's Left button (BOOT on the dev board).
void InitFido(const char *dir) {
    static const buttons::Button presence = boards::Button(boards::ButtonId::kLeft);
    presence.Init();
    static char store[64];
    const bool have_fs = dir != nullptr && snprintf(store, sizeof store, "%s/fido.bin", dir) <
                                               static_cast<int>(sizeof store);
    // The only transport today is BLE: the creator of a credential is the host
    // on the link.
    const fido_ctap_hooks_t hooks = {
        ble_fido::Keepalive, ble_fido::Cancelled, [] { return presence.Pressed(); },
        [](fido_ctap_client_t *out) {
            out->transport = FIDO_CTAP_TRANSPORT_BLE;
            return ble::PeerIdentity(&out->addr_type, out->addr);
        }};
    if (fido_ctap_init(&hooks, have_fs ? store : nullptr, boards::kBoard.fido_max_credentials) ==
        ESP_OK) {
        ble_fido::SetMessageHandler(fido_ctap_request);
    } else {
        ESP_LOGE(TAG, "fido: CTAP core init failed");
    }
}

// Passwords. Unlocked at boot with the interim key until the PIN exists.
void InitVault(const char *dir) {
    crypto::Init();
    if (vault::Init(dir, boards::kBoard.max_records) != ESP_OK || dir == nullptr) {
        return;
    }
    static uint8_t key[vault::kKeySize];  // not on main's stack
    if (vault::InterimKey(key) == ESP_OK) {
        vault::Unlock(key);
    } else {
        ESP_LOGE(TAG, "vault: no key, the password store stays locked");
    }
    vault::Wipe(key, sizeof key);
}

}  // namespace

extern "C" void app_main() {
    boards::Init();  // LED red; the board's clock seeds the system clock (UTC)
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
    ble_fido::Register();
    const char *dir = StorageDir();
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
