#include "usb_fido.h"

#include <atomic>
#include <cstring>

extern "C" {
#include "ctaphid.h"  // C, no guards of its own
}
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"

namespace usb::fido {

namespace {

constexpr const char *TAG = "usb_fido";

// The link task takes the reports (channels, PING, WINK, CANCEL, BUSY to
// other channels); the worker runs the handler, which may wait for the user.
constexpr uint32_t kLinkStack = 4 * 1024;
constexpr UBaseType_t kLinkPriority = 6;
constexpr uint32_t kWorkerStack = 12 * 1024;  // the CTAP core: as ble_fido's
constexpr UBaseType_t kWorkerPriority = 5;
constexpr UBaseType_t kQueueLength = 16;
// The link task looks for a stalled transaction this often.
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(100);
// One IN report waits this long for the endpoint (the host polls every 5 ms).
constexpr TickType_t kSendTicks = pdMS_TO_TICKS(250);
// Start waits this long for a request cancelled by Stop to end.
constexpr int kStartWaitTicks = pdMS_TO_TICKS(2000);

// CTAPHID_INIT's answer: WINK and CBOR; MSG (U2F) too, so no NMSG.
constexpr uint8_t kCapabilities = CTAPHID_CAPABILITY_WINK | CTAPHID_CAPABILITY_CBOR;
constexpr uint8_t kVersionMajor = 1;
constexpr uint8_t kVersionMinor = 0;
constexpr uint8_t kVersionBuild = 0;

struct Report {
    uint8_t data[CTAPHID_PACKET_SIZE];
};

size_t DefaultHandler(const uint8_t *msg, size_t len, uint8_t *out, size_t) {
    // U2F APDUs start with CLA 0x00 (CTAP2 command bytes start at 0x01).
    if (len >= 4 && msg[0] == 0x00) {
        out[0] = 0x6D;  // SW_INS_NOT_SUPPORTED
        out[1] = 0x00;
        return 2;
    }
    out[0] = CTAP1_ERR_INVALID_COMMAND;
    return 1;
}

MessageHandler handler = DefaultHandler;

// PSRAM: a 7.6 KB request buffer each.
ctaphid_state_t *state = nullptr;  // under state_lock; the message itself stays put while busy
uint8_t *response = nullptr;       // the worker's
QueueHandle_t reports = nullptr;
SemaphoreHandle_t state_lock = nullptr;
SemaphoreHandle_t send_lock = nullptr;  // one report at a time; Stop waits for it
SemaphoreHandle_t sent = nullptr;       // given when an IN report went out
TaskHandle_t worker = nullptr;
std::atomic<bool> active{false};  // the interface is up
std::atomic<bool> busy{false};    // the worker has a message
std::atomic<bool> lost{false};    // the interface went down during it
std::atomic<uint32_t> dropped{0};
uint32_t requests = 0;
uint32_t errors = 0;
uint32_t work_cid = 0;  // the worker's message: its channel and command
uint8_t work_cmd = 0;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// False: the interface is down, or the host did not take it in time.
bool Send(const ctaphid_packet_t *packet) {
    xSemaphoreTake(send_lock, portMAX_DELAY);
    const TickType_t start = xTaskGetTickCount();
    while (active && tud_mounted() && !tud_hid_ready() && xTaskGetTickCount() - start < kSendTicks) {
        xSemaphoreTake(sent, 1);
    }
    const bool ok = active && tud_hid_ready() && tud_hid_report(0, packet, CTAPHID_PACKET_SIZE);
    xSemaphoreGive(send_lock);
    return ok;
}

void SendPacket(const ctaphid_packet_t *packet, void *) { Send(packet); }

void SendError(uint32_t cid, uint8_t code) {
    ctaphid_packet_t packet;
    ctaphid_create_error_packet(&packet, cid, code);
    Send(&packet);
    ++errors;
}

void SendInit(const ctaphid_packet_t *request, uint32_t transport_cid, uint32_t new_cid) {
    ctaphid_packet_t packet;
    ctaphid_create_ctaphid_init_response_packet(&packet, request->pkt.init.payload, transport_cid,
                                                new_cid, kVersionMajor, kVersionMinor,
                                                kVersionBuild, kCapabilities);
    Send(&packet);
}

// A whole message (the link task, state_lock held).
void Dispatch() {
    const ctaphid_channel_buffer_t &m = state->buffer;
    switch (m.cmd) {
        case CTAPHID_PING:
            ctaphid_message_to_packets(m.cid, CTAPHID_PING, m.payload_length, m.payload,
                                       SendPacket, nullptr);
            break;
        case CTAPHID_WINK:
            if (m.payload_length != 0) {
                SendError(m.cid, CTAP1_ERR_INVALID_LENGTH);
            } else {
                ctaphid_packet_t packet;
                ctaphid_create_init_packet(&packet, m.cid, CTAPHID_WINK, 0);
                Send(&packet);
            }
            break;
        case CTAPHID_CBOR:
        case CTAPHID_MSG:
            if (m.payload_length == 0) {
                SendError(m.cid, CTAP1_ERR_INVALID_LENGTH);
                break;
            }
            // The buffer stays full (other channels: BUSY) until the worker is done.
            work_cid = m.cid;
            work_cmd = m.cmd;
            lost = false;
            busy = true;
            xTaskNotifyGive(worker);
            return;
        default:
            SendError(m.cid, CTAP1_ERR_INVALID_COMMAND);  // LOCK and the vendor range too
            break;
    }
    ctaphid_reset_to_idle(state);
}

void Process(const ctaphid_packet_t *packet) {
    uint8_t code = CTAP1_ERR_OTHER;
    switch (ctaphid_process_packet(state, packet, NowMs(), &code)) {
        case CTAPHID_RESULT_ERROR:
            SendError(packet->cid, code);
            break;
        case CTAPHID_RESULT_ALLOCATE_CHANNEL: {
            const uint32_t cid = ctaphid_allocate_channel(state);
            if (cid == 0) {
                SendError(packet->cid, CTAP1_ERR_OTHER);  // 2^32 - 2 channels used up
            } else {
                SendInit(packet, CTAPHID_BROADCAST_CID, cid);
            }
            break;
        }
        case CTAPHID_RESULT_DISCARD_INCOMPLETE_MESSAGE:
            SendInit(packet, packet->cid, packet->cid);  // the channel resynchronised
            break;
        case CTAPHID_RESULT_MESSAGE:
            Dispatch();
            break;
        case CTAPHID_RESULT_IGNORED:
        case CTAPHID_RESULT_BUFFERING:
        case CTAPHID_RESULT_CANCEL:  // the flag is in the buffer: Cancelled()
            break;
    }
}

void LinkTask(void *) {
    Report report;
    for (;;) {
        const bool got = xQueueReceive(reports, &report, kPollTicks) == pdTRUE;
        if (!active) {
            continue;
        }
        xSemaphoreTake(state_lock, portMAX_DELAY);
        // 11.2.5.2: a message not finished in time is dropped, its channel told.
        if (ctaphid_has_incomplete_message_timeout(state, NowMs())) {
            SendError(state->buffer.cid, CTAP1_ERR_TIMEOUT);
            ctaphid_reset_to_idle(state);
        }
        if (got) {
            Process(reinterpret_cast<const ctaphid_packet_t *>(report.data));
        }
        xSemaphoreGive(state_lock);
    }
}

void Worker(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const size_t n = handler(state->buffer.payload, state->buffer.payload_length, response,
                                 CTAPHID_MAX_PAYLOAD_LENGTH);
        ++requests;
        if (!lost) {
            ctaphid_message_to_packets(work_cid, work_cmd, n, response, SendPacket, nullptr);
        }
        xSemaphoreTake(state_lock, portMAX_DELAY);
        ctaphid_reset_to_idle(state);
        busy = false;
        xSemaphoreGive(state_lock);
    }
}

void *Allocate(size_t size) {
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != nullptr ? p : heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
}

bool Prepare() {
    state = static_cast<ctaphid_state_t *>(Allocate(sizeof(ctaphid_state_t)));
    response = static_cast<uint8_t *>(Allocate(CTAPHID_MAX_PAYLOAD_LENGTH));
    reports = xQueueCreate(kQueueLength, sizeof(Report));
    state_lock = xSemaphoreCreateMutex();
    send_lock = xSemaphoreCreateMutex();
    sent = xSemaphoreCreateBinary();
    if (state == nullptr || response == nullptr || reports == nullptr || state_lock == nullptr ||
        send_lock == nullptr || sent == nullptr ||
        xTaskCreate(Worker, "usb_fido", kWorkerStack, nullptr, kWorkerPriority, &worker) != pdPASS ||
        // The link's stack in PSRAM: it never touches the flash (the worker does).
        xTaskCreateWithCaps(LinkTask, "usb_ctaphid", kLinkStack, nullptr, kLinkPriority, nullptr,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no memory: no FIDO2 over USB");
        return false;
    }
    return true;
}

}  // namespace

void SetMessageHandler(MessageHandler h) { handler = h != nullptr ? h : DefaultHandler; }

void Keepalive(uint8_t status) {
    if (!busy || lost) {
        return;
    }
    ctaphid_packet_t packet;
    ctaphid_create_init_packet(&packet, work_cid, CTAPHID_KEEPALIVE, 1);
    packet.pkt.init.payload[0] = status;
    Send(&packet);
}

bool Cancelled() { return lost || (state != nullptr && state->buffer.cancel); }

bool InWorker() { return worker != nullptr && xTaskGetCurrentTaskHandle() == worker; }

void Stats(uint32_t *out_requests, uint32_t *out_errors, uint32_t *out_dropped) {
    *out_requests = requests;
    *out_errors = errors;
    *out_dropped = dropped;
}

void Start() {
    static bool prepared = false;
    if (!prepared) {
        if (!Prepare()) {
            return;
        }
        prepared = true;
    }
    // A request cancelled by the last Stop ends first: it still reads the buffer.
    for (int waited = 0; busy && waited < kStartWaitTicks; ++waited) {
        vTaskDelay(1);
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    xQueueReset(reports);
    ctaphid_init(state);
    lost = false;
    active = true;
    xSemaphoreGive(state_lock);
}

void Stop() {
    if (send_lock == nullptr) {
        return;
    }
    xSemaphoreTake(send_lock, portMAX_DELAY);  // no report half-way
    active = false;
    if (busy) {
        lost = true;  // the core sees it as a cancel
    }
    xSemaphoreGive(send_lock);
}

void OnReport(const uint8_t *data, size_t len) {
    if (!active || reports == nullptr) {
        return;
    }
    Report report = {};
    memcpy(report.data, data, len < sizeof report.data ? len : sizeof report.data);
    if (xQueueSend(reports, &report, 0) != pdTRUE) {
        ++dropped;
    }
}

void OnSent() {
    if (sent != nullptr) {
        xSemaphoreGive(sent);
    }
}

}  // namespace usb::fido
