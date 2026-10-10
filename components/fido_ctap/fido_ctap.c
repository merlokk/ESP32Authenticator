#include "fido_ctap.h"

#include <string.h>

#include "ctap.h"
#include "ctap_file_storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fido_ctap";

#define MAX_CREDENTIALS 10
// Working copy in RAM: 10 credentials of at most ~635 bytes, the PIN state,
// plus room for items replaced during this boot (dropped at the next one).
#define STORE_SIZE (12 * 1024)
#define UP_TIMEOUT_MS 30000
#define KEEPALIVE_MS 300  // while processing / waiting for the user
#define UP_POLL_MS 20

static fido_ctap_hooks_t hooks;
static bool ready = false;

// Crypto table: ctap_crypto_esp.cpp (crypto driver + lionkey's micro-ecc).
extern const ctap_crypto_t fido_ctap_crypto;

// --- storage

static uint8_t store_memory[STORE_SIZE] __attribute__((aligned(4)));
static ctap_file_storage_context_t storage_ctx = {
    .mem = {.memory_size = sizeof store_memory, .memory = store_memory, .write_index = 0},
    .path = NULL,
    .max_credentials = MAX_CREDENTIALS,
};
static const ctap_storage_t storage = CTAP_FILE_STORAGE_CONST_INIT(&storage_ctx);

static ctap_state_t state = CTAP_STATE_CONST_INIT(&fido_ctap_crypto, &storage);

// --- platform hooks called by the core

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

uint32_t ctap_get_current_time(void) { return now_ms(); }

static ctap_keepalive_status_t last_status;
static uint32_t last_keepalive_ms;

void ctap_send_keepalive_if_needed(ctap_keepalive_status_t current_status) {
    const uint32_t now = now_ms();
    if (current_status == last_status && now - last_keepalive_ms < KEEPALIVE_MS) {
        return;
    }
    last_status = current_status;
    last_keepalive_ms = now;
    if (hooks.keepalive != NULL) {
        hooks.keepalive((uint8_t)current_status);
    }
}

static bool button(void) { return hooks.button_pressed != NULL && hooks.button_pressed(); }

// Waits for a fresh press: a button already held when the request comes in
// does not count.
ctap_user_presence_result_t ctap_wait_for_user_presence(void) {
    ESP_LOGI(TAG, "user presence: press the button");
    ctap_send_keepalive_if_needed(CTAP_STATUS_UPNEEDED);
    const uint32_t start = now_ms();
    bool was_pressed = button();
    while (true) {
        if (hooks.cancelled != NULL && hooks.cancelled()) {
            ESP_LOGI(TAG, "user presence: cancelled");
            return CTAP_UP_RESULT_CANCEL;
        }
        if (now_ms() - start > UP_TIMEOUT_MS) {
            ESP_LOGI(TAG, "user presence: timeout");
            return CTAP_UP_RESULT_TIMEOUT;
        }
        const bool pressed = button();
        if (pressed && !was_pressed) {
            ESP_LOGI(TAG, "user presence: confirmed");
            return CTAP_UP_RESULT_ALLOW;
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(UP_POLL_MS));
        ctap_send_keepalive_if_needed(CTAP_STATUS_UPNEEDED);
    }
}

// --- API

esp_err_t fido_ctap_init(const fido_ctap_hooks_t *h, const char *store_path) {
    hooks = *h;
    storage_ctx.path = store_path;
    if (storage.init(&storage) != CTAP_STORAGE_OK) {
        return ESP_FAIL;
    }
    if (fido_ctap_crypto.init(&fido_ctap_crypto, 0) != CTAP_CRYPTO_OK) {
        return ESP_FAIL;
    }
    ctap_init(&state);
    ready = true;
    ESP_LOGI(TAG, "ready, store %u of %u bytes", (unsigned)storage_ctx.mem.write_index,
             (unsigned)sizeof store_memory);
    return ESP_OK;
}

size_t fido_ctap_request(const uint8_t *msg, size_t len, uint8_t *out, size_t out_size) {
    if (out_size < 2) {
        return 0;
    }
    if (len >= 4 && msg[0] == 0x00) {
        // U2F (CTAP1) APDU: not supported by the core.
        out[0] = 0x6D;  // SW_INS_NOT_SUPPORTED
        out[1] = 0x00;
        return 2;
    }
    if (!ready) {
        out[0] = CTAP1_ERR_OTHER;
        return 1;
    }
    if (len == 0) {
        out[0] = CTAP1_ERR_INVALID_LENGTH;
        return 1;
    }
    ctap_response_t response = {
        .length = 0,
        .data_max_size = out_size - 1,
        .data = out + 1,
    };
    last_status = CTAP_STATUS_PROCESSING;
    last_keepalive_ms = now_ms();
    out[0] = ctap_request(&state, msg[0], len - 1, msg + 1, &response);
    return 1 + response.length;
}
