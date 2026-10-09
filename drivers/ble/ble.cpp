#include "ble.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

extern "C" void ble_store_config_init(void);

namespace ble {

namespace {

constexpr const char *TAG = "ble";

constexpr size_t kMaxServiceTables = 4;
constexpr size_t kMaxListeners = 4;
constexpr size_t kMaxBonds = CONFIG_BT_NIMBLE_MAX_BONDS;

// Appearance: generic keyboard (Bluetooth assigned numbers).
constexpr uint16_t kAppearanceKeyboard = 0x03C1;
constexpr uint16_t kUuidHid = 0x1812;
constexpr uint16_t kUuidFido = 0xFFFD;

const ble_gatt_svc_def *service_tables[kMaxServiceTables];
size_t service_table_count = 0;
Listener listeners[kMaxListeners];
size_t listener_count = 0;
DropListener drop_listeners[kMaxListeners];
size_t drop_listener_count = 0;

bool on = false;
bool synced = false;
SemaphoreHandle_t synced_sem = nullptr;
uint8_t own_addr_type = 0;
uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
uint16_t mtu = BLE_ATT_MTU_DFLT;
int64_t pairing_until_us = 0;
esp_timer_handle_t pairing_timer = nullptr;
bool pairing_in_progress = false;

// Target host: only it may connect (filter accept list) unless a pairing
// window is open. Persisted in NVS.
constexpr const char *kNvsNamespace = "ble";
constexpr const char *kNvsTarget = "target";
bool has_target = false;
ble_addr_t target = {};

void LoadTarget() {
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = sizeof target;
    has_target = nvs_get_blob(nvs, kNvsTarget, &target, &len) == ESP_OK && len == sizeof target;
    nvs_close(nvs);
}

void SaveTarget() {
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (has_target) {
        nvs_set_blob(nvs, kNvsTarget, &target, sizeof target);
    } else {
        nvs_erase_key(nvs, kNvsTarget);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

// Drops the current link on purpose: drop listeners first (key release).
void Drop() {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    for (size_t i = 0; i < drop_listener_count; ++i) {
        drop_listeners[i](conn_handle);
    }
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}
QueueHandle_t pairing_events = nullptr;

void PostPairing(PairingEvent::Type type, uint32_t passkey, const char *peer,
                 const char *reason) {
    if (pairing_events == nullptr) {
        return;
    }
    PairingEvent event = {.type = type, .passkey = passkey, .peer = {}, .reason = reason};
    if (peer != nullptr) {
        strlcpy(event.peer, peer, sizeof event.peer);
    }
    xQueueSend(pairing_events, &event, 0);  // drop if nobody is reading
}

// --- Device Information and Battery services ---------------------------------

constexpr uint16_t kUuidDis = 0x180A;
constexpr uint16_t kUuidManufacturer = 0x2A29;
constexpr uint16_t kUuidModel = 0x2A24;
constexpr uint16_t kUuidFirmware = 0x2A26;
constexpr uint16_t kUuidPnpId = 0x2A50;
constexpr uint16_t kUuidBattery = 0x180F;
constexpr uint16_t kUuidBatteryLevel = 0x2A19;

// PnP ID: vendor ID source USB-IF (2), Espressif VID 0x303A, product, version.
constexpr uint8_t kPnpId[7] = {0x02, 0x3A, 0x30, 0x01, 0x80, 0x00, 0x01};

int AppendString(os_mbuf *om, const char *s) {
    return os_mbuf_append(om, s, strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int AccessDis(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *) {
    const uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);
    switch (uuid) {
        case kUuidManufacturer: return AppendString(ctxt->om, "ESP32Authenticator");
        case kUuidModel: return AppendString(ctxt->om, "XTEINK X4 Pro");
        case kUuidFirmware: return AppendString(ctxt->om, esp_app_get_description()->version);
        case kUuidPnpId:
            return os_mbuf_append(ctxt->om, kPnpId, sizeof kPnpId) == 0
                       ? 0
                       : BLE_ATT_ERR_INSUFFICIENT_RES;
        default: return BLE_ATT_ERR_UNLIKELY;
    }
}

int AccessBattery(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *) {
    // TODO: real level from the CW2017 fuel gauge.
    const uint8_t level = 100;
    return os_mbuf_append(ctxt->om, &level, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// BLE_UUID16_DECLARE takes the address of a temporary, which C++ rejects.
const ble_uuid16_t kDisUuid = BLE_UUID16_INIT(kUuidDis);
const ble_uuid16_t kManufacturerUuid = BLE_UUID16_INIT(kUuidManufacturer);
const ble_uuid16_t kModelUuid = BLE_UUID16_INIT(kUuidModel);
const ble_uuid16_t kFirmwareUuid = BLE_UUID16_INIT(kUuidFirmware);
const ble_uuid16_t kPnpIdUuid = BLE_UUID16_INIT(kUuidPnpId);
const ble_uuid16_t kBatteryUuid = BLE_UUID16_INIT(kUuidBattery);
const ble_uuid16_t kBatteryLevelUuid = BLE_UUID16_INIT(kUuidBatteryLevel);

const ble_gatt_chr_def kDisChrs[] = {
    {.uuid = &kManufacturerUuid.u, .access_cb = AccessDis, .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &kModelUuid.u, .access_cb = AccessDis, .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &kFirmwareUuid.u, .access_cb = AccessDis, .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &kPnpIdUuid.u, .access_cb = AccessDis, .flags = BLE_GATT_CHR_F_READ},
    {},
};

const ble_gatt_chr_def kBatteryChrs[] = {
    {.uuid = &kBatteryLevelUuid.u, .access_cb = AccessBattery,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY},
    {},
};

const ble_gatt_svc_def kCoreServices[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kDisUuid.u, .characteristics = kDisChrs},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kBatteryUuid.u, .characteristics = kBatteryChrs},
    {},
};

// --- GAP --------------------------------------------------------------------

bool PairingOpen() { return esp_timer_get_time() < pairing_until_us; }

void FormatAddr(const uint8_t *val, char *out) {
    // NimBLE stores addresses little-endian.
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", val[5], val[4], val[3], val[2], val[1],
             val[0]);
}

int GapEvent(ble_gap_event *event, void *arg);

const char *AddrTypeName(uint8_t type) {
    switch (type) {
        case BLE_ADDR_PUBLIC: return "public";
        case BLE_ADDR_RANDOM: return "random";
        case BLE_ADDR_PUBLIC_ID: return "public-id";
        case BLE_ADDR_RANDOM_ID: return "random-id";
        default: return "?";
    }
}

void Advertise() {
    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = kAppearanceKeyboard;
    fields.appearance_is_present = 1;
    static const ble_uuid16_t uuids[] = {BLE_UUID16_INIT(kUuidHid), BLE_UUID16_INIT(kUuidFido)};
    fields.uuids16 = uuids;
    fields.num_uuids16 = 2;
    fields.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields: %d", rc);
        return;
    }

    ble_hs_adv_fields rsp = {};
    rsp.name = reinterpret_cast<const uint8_t *>(kDeviceName);
    rsp.name_len = strlen(kDeviceName);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan rsp fields: %d", rc);
        return;
    }

    ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (has_target && !PairingOpen()) {
        // Only the target may scan and connect.
        rc = ble_gap_wl_set(&target, 1);
        if (rc == 0) {
            params.filter_policy = BLE_HCI_ADV_FILT_BOTH;
        } else {
            ESP_LOGE(TAG, "filter accept list: %d", rc);
        }
    }
    rc = ble_gap_adv_start(own_addr_type, nullptr, BLE_HS_FOREVER, &params, GapEvent, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start: %d", rc);
    }
}

// Re-applies the advertising filter while idle (target or window changed).
void RestartAdvertising() {
    if (!on || !synced || conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    ble_gap_adv_stop();
    Advertise();
}

void OnPairingTimer(void *) { RestartAdvertising(); }

void OnPasskeyAction(const ble_gap_event *event) {
    const uint16_t conn = event->passkey.conn_handle;
    if (!PairingOpen()) {
        ESP_LOGW(TAG, "pairing request rejected: pairing window closed (run 'ble pair')");
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        return;
    }
    ble_sm_io io = {};
    io.action = event->passkey.params.action;
    if (io.action == BLE_SM_IOACT_DISP) {
        io.passkey = esp_random() % 1000000;
        pairing_in_progress = true;
        // TODO: show the passkey on the e-ink screen.
        ESP_LOGI(TAG, "pairing passkey: %06" PRIu32, io.passkey);
        PostPairing(PairingEvent::kPasskey, io.passkey, nullptr, nullptr);
    } else {
        // Only DisplayOnly is configured; anything else is unexpected.
        ESP_LOGW(TAG, "unsupported pairing action %d", io.action);
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        return;
    }
    ble_sm_inject_io(conn, &io);
}

void OnEncChange(const ble_gap_event *event) {
    ble_gap_conn_desc desc = {};
    if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0) {
        return;
    }
    const bool was_pairing = pairing_in_progress;
    pairing_in_progress = false;
    if (event->enc_change.status != 0) {
        ESP_LOGW(TAG, "encryption failed: %d", event->enc_change.status);
        if (was_pairing) {
            PostPairing(PairingEvent::kFailed, 0, nullptr, "wrong passkey or host cancelled");
        }
        return;
    }
    if (!desc.sec_state.authenticated) {
        // Just Works fallback (host without a keyboard/display): no MITM protection.
        ESP_LOGW(TAG, "unauthenticated link rejected");
        if (PairingOpen()) {
            PostPairing(PairingEvent::kFailed, 0, nullptr,
                        "host paired without passkey (Just Works), rejected");
        }
        if (desc.sec_state.bonded) {
            ble_gap_unpair(&desc.peer_id_addr);
        }
        ble_gap_terminate(desc.conn_handle, BLE_ERR_AUTH_FAIL);
        return;
    }
    if (was_pairing) {
        char addr[18];
        FormatAddr(desc.peer_id_addr.val, addr);
        ESP_LOGI(TAG, "paired with %s", addr);
        pairing_until_us = 0;  // one new bond per window
        if (has_target) {
            target = desc.peer_id_addr;  // the new host is the one wanted now
            SaveTarget();
        }
        PostPairing(PairingEvent::kPaired, 0, addr, nullptr);
        return;
    }
    if (has_target && ble_addr_cmp(&desc.peer_id_addr, &target) != 0) {
        // Backstop for the accept list (e.g. a host on a resolvable address).
        ESP_LOGW(TAG, "not the target host, dropping");
        ble_gap_terminate(desc.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int GapEvent(ble_gap_event *event, void *) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                mtu = BLE_ATT_MTU_DFLT;
                // Encrypt right away: bonded hosts re-encrypt, new ones pair.
                ble_gap_security_initiate(conn_handle);
            } else {
                Advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (pairing_in_progress) {
                PostPairing(PairingEvent::kFailed, 0, nullptr, "host disconnected during pairing");
            }
            pairing_in_progress = false;
            if (on) {
                Advertise();
            }
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (on && conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                Advertise();
            }
            break;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            OnPasskeyAction(event);
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            OnEncChange(event);
            break;
        case BLE_GAP_EVENT_MTU:
            mtu = event->mtu.value;
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // The host lost its keys and pairs again: allowed only in the window.
            if (!PairingOpen()) {
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            ble_gap_conn_desc desc = {};
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        default:
            break;
    }
    for (size_t i = 0; i < listener_count; ++i) {
        listeners[i](event);
    }
    return 0;
}

void OnSync() {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    synced = true;
    Advertise();
    xSemaphoreGive(synced_sem);
}

void OnReset(int reason) {
    synced = false;
    ESP_LOGW(TAG, "host reset: %d", reason);
}

void HostTask(void *) {
    nimble_port_run();  // returns after nimble_port_stop()
    nimble_port_freertos_deinit();
}

}  // namespace

void AddServices(const ble_gatt_svc_def *services) {
    if (service_table_count < kMaxServiceTables) {
        service_tables[service_table_count++] = services;
    }
}

void AddListener(Listener listener) {
    if (listener_count < kMaxListeners) {
        listeners[listener_count++] = listener;
    }
}

void AddDropListener(DropListener listener) {
    if (drop_listener_count < kMaxListeners) {
        drop_listeners[drop_listener_count++] = listener;
    }
}

esp_err_t On() {
    if (on) {
        return ESP_OK;
    }
    if (synced_sem == nullptr) {
        synced_sem = xSemaphoreCreateBinary();
        const esp_timer_create_args_t args = {.callback = OnPairingTimer, .name = "ble_pair"};
        esp_timer_create(&args, &pairing_timer);
    }
    LoadTarget();
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(kDeviceName);
    ble_svc_gap_device_appearance_set(kAppearanceKeyboard);

    int rc = ble_gatts_count_cfg(kCoreServices);
    rc = rc == 0 ? ble_gatts_add_svcs(kCoreServices) : rc;
    for (size_t i = 0; rc == 0 && i < service_table_count; ++i) {
        rc = ble_gatts_count_cfg(service_tables[i]);
        rc = rc == 0 ? ble_gatts_add_svcs(service_tables[i]) : rc;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt services: %d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    ble_store_config_init();
    on = true;
    nimble_port_freertos_init(HostTask);
    if (xSemaphoreTake(synced_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "host did not sync in 3 s");
    }
    return ESP_OK;
}

esp_err_t Off() {
    if (!on) {
        return ESP_OK;
    }
    pairing_until_us = 0;
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        Drop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    on = false;
    ble_gap_adv_stop();
    if (nimble_port_stop() != 0) {
        ESP_LOGE(TAG, "nimble stop failed");
        return ESP_FAIL;
    }
    nimble_port_deinit();
    synced = false;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    return ESP_OK;
}

bool IsOn() { return on; }

esp_err_t StartPairing(uint32_t seconds) {
    if (!on) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pairing_events == nullptr) {
        pairing_events = xQueueCreate(4, sizeof(PairingEvent));
    }
    xQueueReset(pairing_events);
    pairing_until_us = esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000000;
    RestartAdvertising();  // open to everyone for the window
    esp_timer_stop(pairing_timer);
    esp_timer_start_once(pairing_timer, static_cast<uint64_t>(seconds) * 1000000 + 100000);
    return ESP_OK;
}

bool WaitPairingEvent(PairingEvent *event, uint32_t timeout_ms) {
    return pairing_events != nullptr &&
           xQueueReceive(pairing_events, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StopPairing() {
    pairing_until_us = 0;
    esp_timer_stop(pairing_timer);
    RestartAdvertising();
}

uint32_t PairingSecondsLeft() {
    const int64_t left = pairing_until_us - esp_timer_get_time();
    return left > 0 ? static_cast<uint32_t>((left + 999999) / 1000000) : 0;
}

namespace {

// Bond by number from PrintBonds() (1-based) or by address.
esp_err_t FindBond(const char *which, ble_addr_t *out) {
    ble_addr_t peers[kMaxBonds];
    int count = 0;
    ble_store_util_bonded_peers(peers, &count, kMaxBonds);
    char *end = nullptr;
    const long index = strtol(which, &end, 10);
    if (end != which && *end == '\0') {
        if (index < 1 || index > count) {
            return ESP_ERR_NOT_FOUND;
        }
        *out = peers[index - 1];
        return ESP_OK;
    }
    unsigned b[6];
    if (sscanf(which, "%x:%x:%x:%x:%x:%x", &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < count; ++i) {
        bool match = true;
        for (int j = 0; j < 6; ++j) {
            match = match && peers[i].val[j] == b[j];
        }
        if (match) {
            *out = peers[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

bool IsConnectedTo(const ble_addr_t &addr) {
    ble_gap_conn_desc desc = {};
    return conn_handle != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn_handle, &desc) == 0 &&
           ble_addr_cmp(&desc.peer_id_addr, &addr) == 0;
}

}  // namespace

esp_err_t Unpair(const char *which) {
    if (!on) {
        return ESP_ERR_INVALID_STATE;  // the bond store lives in the host
    }
    if (which == nullptr) {
        Drop();
        has_target = false;
        SaveTarget();
        return ble_store_clear() == 0 ? ESP_OK : ESP_FAIL;
    }
    ble_addr_t addr;
    const esp_err_t err = FindBond(which, &addr);
    if (err != ESP_OK) {
        return err;
    }
    if (IsConnectedTo(addr)) {
        Drop();
    }
    if (has_target && ble_addr_cmp(&addr, &target) == 0) {
        has_target = false;
        SaveTarget();
        RestartAdvertising();
    }
    return ble_gap_unpair(&addr) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t UseBond(const char *which) {
    if (!on) {
        return ESP_ERR_INVALID_STATE;
    }
    if (which == nullptr) {
        has_target = false;
    } else {
        ble_addr_t addr;
        const esp_err_t err = FindBond(which, &addr);
        if (err != ESP_OK) {
            return err;
        }
        target = addr;
        has_target = true;
    }
    SaveTarget();
    if (has_target && conn_handle != BLE_HS_CONN_HANDLE_NONE && !IsConnectedTo(target)) {
        Drop();  // advertising restarts with the filter on disconnect
    } else {
        RestartAdvertising();
    }
    return ESP_OK;
}

esp_err_t Disconnect() {
    if (!on || conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    Drop();
    return ESP_OK;
}

uint16_t ConnHandle() { return conn_handle; }

bool LinkSecure() {
    ble_gap_conn_desc desc = {};
    return conn_handle != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn_handle, &desc) == 0 &&
           desc.sec_state.encrypted && desc.sec_state.authenticated;
}

uint16_t NotifyPayload() { return mtu - 3; }

void PrintInfo() {
    printf("state      %s\n", on ? (synced ? "on" : "on (not synced)") : "off");
    printf("name       %s\n", kDeviceName);
    if (!on) {
        return;
    }
    uint8_t addr_val[6] = {};
    char addr[18];
    if (ble_hs_id_copy_addr(own_addr_type, addr_val, nullptr) == 0) {
        FormatAddr(addr_val, addr);
        printf("address    %s (%s)\n", addr, own_addr_type == BLE_OWN_ADDR_PUBLIC ? "public" : "random");
    }
    printf("advertising %s\n", ble_gap_adv_active() ? "yes" : "no");
    const uint32_t left = PairingSecondsLeft();
    if (left > 0) {
        printf("pairing    open, %" PRIu32 " s left\n", left);
    } else {
        printf("pairing    closed\n");
    }

    if (has_target) {
        FormatAddr(target.val, addr);
        printf("target     %s (only it may connect)\n", addr);
    } else {
        printf("target     any bonded host\n");
    }
    ble_gap_conn_desc desc = {};
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn_handle, &desc) == 0) {
        FormatAddr(desc.peer_id_addr.val, addr);
        printf("connected  %s ('ble conns' for details)\n", addr);
    } else {
        printf("connected  no\n");
    }
    int count = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
    printf("bonds      %d ('ble bonds' to list)\n", count);
}

void PrintBonds() {
    if (!on) {
        printf("ble is off\n");
        return;
    }
    ble_addr_t peers[kMaxBonds];
    int count = 0;
    ble_store_util_bonded_peers(peers, &count, kMaxBonds);
    if (count == 0) {
        printf("no bonds ('ble pair' to add one)\n");
        return;
    }
    ble_gap_conn_desc desc = {};
    const bool connected =
        conn_handle != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn_handle, &desc) == 0;
    printf(" #  address            type\n");
    for (int i = 0; i < count; ++i) {
        char addr[18];
        FormatAddr(peers[i].val, addr);
        const bool here = connected && ble_addr_cmp(&peers[i], &desc.peer_id_addr) == 0;
        const bool wanted = has_target && ble_addr_cmp(&peers[i], &target) == 0;
        printf("%2d  %s  %-9s%s%s\n", i + 1, addr, AddrTypeName(peers[i].type),
               here ? "  connected" : "", wanted ? "  target" : "");
    }
    printf("%d bond(s), max %d\n", count, CONFIG_BT_NIMBLE_MAX_BONDS);
}

void PrintConnection() {
    if (!on) {
        printf("ble is off\n");
        return;
    }
    ble_gap_conn_desc desc = {};
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(conn_handle, &desc) != 0) {
        printf("no connections\n");
        return;
    }
    char addr[18];
    printf("handle     %u\n", desc.conn_handle);
    FormatAddr(desc.peer_id_addr.val, addr);
    printf("peer       %s (%s)\n", addr, AddrTypeName(desc.peer_id_addr.type));
    FormatAddr(desc.peer_ota_addr.val, addr);
    printf("peer ota   %s (%s)\n", addr, AddrTypeName(desc.peer_ota_addr.type));
    printf("security   %s%s%s, key %u bytes\n",
           desc.sec_state.encrypted ? "encrypted" : "not encrypted",
           desc.sec_state.authenticated ? ", authenticated" : "",
           desc.sec_state.bonded ? ", bonded" : "", desc.sec_state.key_size);
    printf("mtu        %u\n", mtu);
    // Interval in 1.25 ms units, supervision timeout in 10 ms units.
    printf("interval   %u.%02u ms, latency %u, timeout %u ms\n", desc.conn_itvl * 125 / 100,
           desc.conn_itvl * 125 % 100, desc.conn_latency, desc.supervision_timeout * 10);
    int8_t rssi = 0;
    if (ble_gap_conn_rssi(desc.conn_handle, &rssi) == 0) {
        printf("rssi       %d dBm\n", rssi);
    }
}

}  // namespace ble
