#include "ble_kb.h"

#include <cstring>

#include "ble.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"

namespace ble_kb {

namespace {

constexpr const char *TAG = "ble_kb";

constexpr uint8_t kReportId = 1;
constexpr uint8_t kReportTypeInput = 1;
constexpr uint8_t kReportTypeOutput = 2;
constexpr uint8_t kShift = 0x02;  // left shift modifier bit
constexpr int kKeyDelayMs = 10;

// Keyboard with report ID 1: 8-byte input (modifiers, reserved, 6 keys) and
// 1-byte LED output.
constexpr uint8_t kReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, kReportId,  // Usage Page Generic Desktop, Keyboard
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,       // modifiers E0..E7
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                   // reserved byte
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,       // 5 LEDs
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x91, 0x01,                                           // LED padding
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,       // 6 key codes
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
};

// bcdHID 1.11, country 0, flags: normally connectable.
constexpr uint8_t kHidInfo[] = {0x11, 0x01, 0x00, 0x02};

const ble_uuid16_t kHidUuid = BLE_UUID16_INIT(0x1812);
const ble_uuid16_t kHidInfoUuid = BLE_UUID16_INIT(0x2A4A);
const ble_uuid16_t kReportMapUuid = BLE_UUID16_INIT(0x2A4B);
const ble_uuid16_t kControlPointUuid = BLE_UUID16_INIT(0x2A4C);
const ble_uuid16_t kReportUuid = BLE_UUID16_INIT(0x2A4D);
const ble_uuid16_t kProtocolModeUuid = BLE_UUID16_INIT(0x2A4E);
const ble_uuid16_t kReportRefUuid = BLE_UUID16_INIT(0x2908);

uint16_t input_handle = 0;
bool subscribed = false;
uint8_t protocol_mode = 1;  // report protocol
uint8_t leds = 0;

int Append(os_mbuf *om, const void *data, size_t len) {
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int AccessHid(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *arg) {
    const uintptr_t which = reinterpret_cast<uintptr_t>(arg);
    switch (which) {
        case 0x2A4A: return Append(ctxt->om, kHidInfo, sizeof kHidInfo);
        case 0x2A4B: return Append(ctxt->om, kReportMap, sizeof kReportMap);
        case 0x2A4C: return 0;  // suspend / exit suspend: nothing to do
        case 0x2A4E:
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                uint16_t len = 0;
                ble_hs_mbuf_to_flat(ctxt->om, &protocol_mode, 1, &len);
                return 0;
            }
            return Append(ctxt->om, &protocol_mode, 1);
        case 1: {  // input report
            const uint8_t empty[8] = {};
            return Append(ctxt->om, empty, sizeof empty);
        }
        case 2:  // output report (LEDs)
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                uint16_t len = 0;
                ble_hs_mbuf_to_flat(ctxt->om, &leds, 1, &len);
                return 0;
            }
            return Append(ctxt->om, &leds, 1);
        default: return BLE_ATT_ERR_UNLIKELY;
    }
}

int AccessReportRef(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *arg) {
    const uint8_t ref[2] = {kReportId, static_cast<uint8_t>(reinterpret_cast<uintptr_t>(arg))};
    return Append(ctxt->om, ref, sizeof ref);
}

void *Arg(uintptr_t v) { return reinterpret_cast<void *>(v); }

ble_gatt_dsc_def kInputRef[] = {
    {.uuid = &kReportRefUuid.u, .att_flags = BLE_ATT_F_READ, .access_cb = AccessReportRef,
     .arg = Arg(kReportTypeInput)},
    {},
};

ble_gatt_dsc_def kOutputRef[] = {
    {.uuid = &kReportRefUuid.u, .att_flags = BLE_ATT_F_READ, .access_cb = AccessReportRef,
     .arg = Arg(kReportTypeOutput)},
    {},
};

// HID needs an encrypted, authenticated link: the report characteristics are
// readable/writable only that way.
ble_gatt_chr_def kHidChrs[] = {
    {.uuid = &kHidInfoUuid.u, .access_cb = AccessHid, .arg = Arg(0x2A4A),
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &kReportMapUuid.u, .access_cb = AccessHid, .arg = Arg(0x2A4B),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN},
    {.uuid = &kControlPointUuid.u, .access_cb = AccessHid, .arg = Arg(0x2A4C),
     .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
    {.uuid = &kProtocolModeUuid.u, .access_cb = AccessHid, .arg = Arg(0x2A4E),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP},
    {.uuid = &kReportUuid.u, .access_cb = AccessHid, .arg = Arg(1), .descriptors = kInputRef,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &input_handle},
    {.uuid = &kReportUuid.u, .access_cb = AccessHid, .arg = Arg(2), .descriptors = kOutputRef,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_WRITE |
              BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_AUTHEN},
    {},
};

const ble_gatt_svc_def kServices[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kHidUuid.u, .characteristics = kHidChrs},
    {},
};

void OnEvent(const ble_gap_event *event) {
    if (event->type == BLE_GAP_EVENT_SUBSCRIBE && event->subscribe.attr_handle == input_handle) {
        subscribed = event->subscribe.cur_notify;
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        subscribed = false;
    }
}

// US layout: HID usage of `c` and whether it needs Shift. Returns false if
// `c` cannot be typed.
bool KeyFor(char c, uint8_t *usage, bool *shift) {
    static const char kShifted[] = "!@#$%^&*()";
    static const char kPlain[] = "-=[]\\;'`,./";
    static const char kPlainShifted[] = "_+{}|:\"~<>?";
    static const uint8_t kPlainUsage[] = {0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33,
                                          0x34, 0x35, 0x36, 0x37, 0x38};
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        *usage = 0x04 + (c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
        *usage = 0x04 + (c - 'A');
        *shift = true;
    } else if (c >= '1' && c <= '9') {
        *usage = 0x1E + (c - '1');
    } else if (c == '0') {
        *usage = 0x27;
    } else if (c == '\n') {
        *usage = 0x28;
    } else if (c == '\t') {
        *usage = 0x2B;
    } else if (c == ' ') {
        *usage = 0x2C;
    } else if (const char *p = strchr(kShifted, c); p != nullptr && c != '\0') {
        *usage = 0x1E + (p - kShifted);  // '!'..'(' are Shift+1..9, ')' is Shift+0
        *shift = true;
    } else if (const char *q = strchr(kPlain, c); q != nullptr && c != '\0') {
        *usage = kPlainUsage[q - kPlain];
    } else if (const char *r = strchr(kPlainShifted, c); r != nullptr && c != '\0') {
        *usage = kPlainUsage[r - kPlainShifted];
        *shift = true;
    } else {
        return false;
    }
    return true;
}

esp_err_t SendReport(uint8_t modifiers, uint8_t usage) {
    const uint8_t report[8] = {modifiers, 0, usage, 0, 0, 0, 0, 0};
    for (int attempt = 0; attempt < 50; ++attempt) {
        os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof report);
        if (om == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const int rc = ble_gatts_notify_custom(ble::ConnHandle(), input_handle, om);
        if (rc == 0) {
            return ESP_OK;
        }
        if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
            ESP_LOGE(TAG, "notify: %d", rc);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}

}  // namespace

void Register() {
    ble::AddServices(kServices);
    ble::AddListener(OnEvent);
}

bool Ready() { return subscribed && ble::LinkSecure(); }

esp_err_t Type(const char *text) {
    if (!Ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t usage = 0;
    bool shift = false;
    for (const char *c = text; *c != '\0'; ++c) {
        if (!KeyFor(*c, &usage, &shift)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (const char *c = text; *c != '\0'; ++c) {
        KeyFor(*c, &usage, &shift);
        esp_err_t err = SendReport(shift ? kShift : 0, usage);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(kKeyDelayMs));
            err = SendReport(0, 0);  // release
        }
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(kKeyDelayMs));
    }
    return ESP_OK;
}

}  // namespace ble_kb
