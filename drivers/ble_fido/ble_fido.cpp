#include "ble_fido.h"

#include <cstring>

#include <atomic>

#include "ble.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"

namespace ble_fido {

namespace {

constexpr const char *TAG = "ble_fido";

// Frame commands (CTAP 2.1, 11.4 "Bluetooth Smart / BLE").
constexpr uint8_t kCmdPing = 0x81;
constexpr uint8_t kCmdKeepalive = 0x82;
constexpr uint8_t kCmdMsg = 0x83;
constexpr uint8_t kCmdCancel = 0xBE;
constexpr uint8_t kCmdError = 0xBF;

constexpr uint8_t kErrInvalidCmd = 0x01;
constexpr uint8_t kErrInvalidLen = 0x03;
constexpr uint8_t kErrInvalidSeq = 0x04;
constexpr uint8_t kErrBusy = 0x06;

// The message handler may wait for the user (CTAP user presence), so it runs
// in its own task, not in the NimBLE host task.
constexpr uint32_t kWorkerStack = 12 * 1024;
constexpr UBaseType_t kWorkerPriority = 5;

// fidoServiceRevisionBitfield: bit 5 = FIDO2 (CTAP2) over BLE.
constexpr uint8_t kRevisionFido2 = 0x20;

// Max size of one fidoControlPoint write (20..512).
constexpr uint16_t kControlPointLength = 512;
// Max assembled request / response (CTAP2 messages are small; 7609 is the limit).
constexpr size_t kMaxMessage = 2048;

// F1D0FFF1..4-DEAA-ECEE-B42F-C9BA7ED623BB, little-endian.
#define FIDO_UUID128(n) \
    BLE_UUID128_INIT(0xbb, 0x23, 0xd6, 0x7e, 0xba, 0xc9, 0x2f, 0xb4, 0xee, 0xec, 0xaa, 0xde, \
                     n, 0xff, 0xd0, 0xf1)
const ble_uuid16_t kServiceUuid = BLE_UUID16_INIT(0xFFFD);
const ble_uuid128_t kControlPointUuid = FIDO_UUID128(0xf1);
const ble_uuid128_t kStatusUuid = FIDO_UUID128(0xf2);
const ble_uuid128_t kControlPointLengthUuid = FIDO_UUID128(0xf3);
const ble_uuid128_t kRevisionBitfieldUuid = FIDO_UUID128(0xf4);
#undef FIDO_UUID128

uint16_t status_handle = 0;
bool subscribed = false;
uint8_t revision = kRevisionFido2;

// Request being assembled. Written from the host task only.
uint8_t request[kMaxMessage];
uint8_t response[kMaxMessage];
uint8_t request_cmd = 0;
size_t request_len = 0;       // announced total length
size_t request_got = 0;       // bytes received so far
uint8_t next_seq = 0;
bool assembling = false;

uint32_t requests = 0;
uint32_t errors = 0;

// MSG being handled by the worker: copied out of `request`, which the host
// task keeps using for the next frames.
uint8_t work[kMaxMessage];
size_t work_len = 0;
std::atomic<bool> busy{false};
std::atomic<bool> cancelled{false};
TaskHandle_t worker = nullptr;
SemaphoreHandle_t send_lock = nullptr;  // one response's frames go out together

size_t DefaultHandler(const uint8_t *msg, size_t len, uint8_t *out, size_t) {
    // U2F APDUs start with CLA 0x00 (CTAP2 command bytes start at 0x01).
    if (len >= 4 && msg[0] == 0x00) {
        out[0] = 0x6D;  // SW_INS_NOT_SUPPORTED
        out[1] = 0x00;
        return 2;
    }
    out[0] = 0x01;  // CTAP1_ERR_INVALID_COMMAND
    return 1;
}

MessageHandler handler = DefaultHandler;

// Sends one response as fidoStatus notifications, fragmented by MTU.
// Called from the host task and the worker.
void Send(uint8_t cmd, const uint8_t *data, size_t len) {
    xSemaphoreTake(send_lock, portMAX_DELAY);
    struct Unlock {
        ~Unlock() { xSemaphoreGive(send_lock); }
    } unlock;
    const uint16_t conn = ble::ConnHandle();
    size_t frame_max = ble::NotifyPayload();
    if (frame_max > kControlPointLength) {
        frame_max = kControlPointLength;
    }
    static uint8_t frame[kControlPointLength];  // host task only; keep it off its stack
    frame[0] = cmd;
    frame[1] = static_cast<uint8_t>(len >> 8);
    frame[2] = static_cast<uint8_t>(len);
    size_t take = len < frame_max - 3 ? len : frame_max - 3;
    memcpy(frame + 3, data, take);
    size_t frame_len = 3 + take;
    size_t sent = take;
    uint8_t seq = 0;
    while (true) {
        os_mbuf *om = ble_hs_mbuf_from_flat(frame, frame_len);
        if (om == nullptr || ble_gatts_notify_custom(conn, status_handle, om) != 0) {
            ESP_LOGE(TAG, "notify failed");
            return;
        }
        if (sent >= len) {
            return;
        }
        frame[0] = seq++ & 0x7F;
        take = len - sent < frame_max - 1 ? len - sent : frame_max - 1;
        memcpy(frame + 1, data + sent, take);
        frame_len = 1 + take;
        sent += take;
    }
}

void SendError(uint8_t code) {
    ++errors;
    assembling = false;
    Send(kCmdError, &code, 1);
}

void Dispatch() {
    assembling = false;
    ++requests;
    switch (request_cmd) {
        case kCmdPing:
            Send(kCmdPing, request, request_len);
            break;
        case kCmdMsg:
            if (busy.exchange(true)) {
                SendError(kErrBusy);
                break;
            }
            memcpy(work, request, request_len);
            work_len = request_len;
            cancelled = false;
            xTaskNotifyGive(worker);
            break;
        case kCmdCancel:
            if (busy) {
                cancelled = true;  // the handler sees it and answers KEEPALIVE_CANCEL
            }
            break;
        default:
            SendError(kErrInvalidCmd);
            break;
    }
}

// One fidoControlPoint write: an initial frame (cmd, len) or a continuation (seq).
int OnControlPoint(const uint8_t *frame, uint16_t len) {
    if (len == 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (frame[0] & 0x80) {
        if (len < 3) {
            SendError(kErrInvalidLen);
            return 0;
        }
        request_cmd = frame[0];
        request_len = (frame[1] << 8) | frame[2];
        if (request_len > sizeof request || len - 3u > request_len) {
            SendError(kErrInvalidLen);
            return 0;
        }
        memcpy(request, frame + 3, len - 3);
        request_got = len - 3;
        next_seq = 0;
        assembling = true;
    } else {
        if (!assembling || frame[0] != next_seq) {
            SendError(kErrInvalidSeq);
            return 0;
        }
        if (request_got + (len - 1u) > request_len) {
            SendError(kErrInvalidLen);
            return 0;
        }
        memcpy(request + request_got, frame + 1, len - 1);
        request_got += len - 1;
        next_seq = (next_seq + 1) & 0x7F;
    }
    if (request_got == request_len) {
        Dispatch();
    }
    return 0;
}

int Access(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *arg) {
    const uintptr_t which = reinterpret_cast<uintptr_t>(arg);
    switch (which) {
        case 1: {  // fidoControlPoint
            static uint8_t frame[kControlPointLength];  // host task only
            uint16_t len = 0;
            if (OS_MBUF_PKTLEN(ctxt->om) > sizeof frame ||
                ble_hs_mbuf_to_flat(ctxt->om, frame, sizeof frame, &len) != 0) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            return OnControlPoint(frame, len);
        }
        case 2:  // fidoStatus: notify only
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        case 3: {  // fidoControlPointLength, big-endian
            const uint8_t v[2] = {kControlPointLength >> 8, kControlPointLength & 0xFF};
            return os_mbuf_append(ctxt->om, v, 2) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case 4:  // fidoServiceRevisionBitfield
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                uint8_t v = 0;
                uint16_t len = 0;
                ble_hs_mbuf_to_flat(ctxt->om, &v, 1, &len);
                if (len != 1 || (v & ~kRevisionFido2) != 0) {
                    return BLE_ATT_ERR_UNLIKELY;  // only FIDO2 is offered
                }
                revision = v;
                return 0;
            }
            return os_mbuf_append(ctxt->om, &revision, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

void *Arg(uintptr_t v) { return reinterpret_cast<void *>(v); }

// FIDO requires an encrypted, authenticated link for every characteristic.
ble_gatt_chr_def kChrs[] = {
    {.uuid = &kControlPointUuid.u, .access_cb = Access, .arg = Arg(1),
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN},
    {.uuid = &kStatusUuid.u, .access_cb = Access, .arg = Arg(2), .flags = BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &status_handle},
    {.uuid = &kControlPointLengthUuid.u, .access_cb = Access, .arg = Arg(3),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN},
    {.uuid = &kRevisionBitfieldUuid.u, .access_cb = Access, .arg = Arg(4),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_WRITE |
              BLE_GATT_CHR_F_WRITE_AUTHEN},
    {},
};

const ble_gatt_svc_def kServices[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kServiceUuid.u, .characteristics = kChrs},
    {},
};

void OnEvent(const ble_gap_event *event) {
    if (event->type == BLE_GAP_EVENT_SUBSCRIBE && event->subscribe.attr_handle == status_handle) {
        subscribed = event->subscribe.cur_notify;
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        subscribed = false;
        assembling = false;
        revision = kRevisionFido2;
        if (busy) {
            cancelled = true;  // nobody waits for the answer any more
        }
    }
}

void Worker(void *) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const size_t n = handler(work, work_len, response, sizeof response);
        Send(kCmdMsg, response, n);
        busy = false;
    }
}

}  // namespace

void Register() {
    send_lock = xSemaphoreCreateMutex();
    xTaskCreate(Worker, "ble_fido", kWorkerStack, nullptr, kWorkerPriority, &worker);
    ble::AddServices(kServices);
    ble::AddListener(OnEvent);
}

void Keepalive(uint8_t status) {
    if (busy && ble::ConnHandle() != BLE_HS_CONN_HANDLE_NONE) {
        Send(kCmdKeepalive, &status, 1);
    }
}

bool Cancelled() { return cancelled; }

void SetMessageHandler(MessageHandler h) { handler = h != nullptr ? h : DefaultHandler; }

bool Ready() { return subscribed && ble::LinkSecure(); }

void Stats(uint32_t *out_requests, uint32_t *out_errors) {
    *out_requests = requests;
    *out_errors = errors;
}

}  // namespace ble_fido
