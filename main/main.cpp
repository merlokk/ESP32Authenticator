#include "ble_fido.h"
#include "ble_kb.h"
#include "ble.h"
#include "buttons.h"
#include "config.h"
#include "console.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "fido_ctap.h"
#include "nvs_flash.h"
#include "spiffs_fs.h"
#include "wifimgr.h"

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

// FIDO2 over BLE: the LionKey CTAP core behind the ble_fido transport. User
// presence: Left button (GPIO0, the BOOT button on the dev board).
void InitFido() {
    buttons::kLeft.Init();
    static char store[64];
    const bool have_fs = spiffs_fs::Mounted() && spiffs_fs::FullPath("fido.bin", store, sizeof store);
    const fido_ctap_hooks_t hooks = {ble_fido::Keepalive, ble_fido::Cancelled,
                                     [] { return buttons::kLeft.Pressed(); }};
    if (fido_ctap_init(&hooks, have_fs ? store : nullptr) == ESP_OK) {
        ble_fido::SetMessageHandler(fido_ctap_request);
    } else {
        ESP_LOGE(TAG, "fido: CTAP core init failed");
    }
}

}  // namespace

extern "C" void app_main() {
    ConfirmRunningApp();
    InitNvs();
    spiffs_fs::Init();  // not fatal: the console reports an unmounted fs
    // config.json from SPIFFS; built-in defaults if missing/invalid or unmounted.
    // TODO: look on the microSD card first.
    config::Init(spiffs_fs::kBasePath);

    ble_kb::Register();  // BLE profiles, before the stack starts
    ble_fido::Register();
    InitFido();
    ESP_ERROR_CHECK(console::Init());

    wifimgr::Start();  // starts the radio if wifi.active
    if (config::Get().ble.active) {
        ble::On();
    }
}
