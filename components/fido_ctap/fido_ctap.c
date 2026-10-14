#include "fido_ctap.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ctap.h"
#include "ctap_credentials_store.h"
#include "ctap_file_storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "fido_ctap";

#define UP_TIMEOUT_MS 30000
#define KEEPALIVE_MS 300  // while processing / waiting for the user
#define UP_POLL_MS 20
#define ADMIN_WAIT_MS 1000  // console vs a running request

static fido_ctap_hooks_t hooks;
static bool ready = false;

// One user of the core and its store at a time: the transport's request task
// and the console. A request holds it for its whole run (button wait included).
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock = NULL;

// Crypto table: ctap_crypto_esp.cpp (crypto driver + lionkey's micro-ecc).
extern const ctap_crypto_t fido_ctap_crypto;

// --- storage

// The RAM log is sized from max_credentials at init and lives in PSRAM. Its
// context has const members (lionkey), so it is built at init on the heap and
// `storage.context` repointed; until then an empty one.
static ctap_file_storage_context_t empty_ctx = {
    .mem = {.memory_size = 0, .memory = NULL, .write_index = 0},
    .path = NULL,
    .max_credentials = 0,
};
static ctap_file_storage_context_t *ctx = &empty_ctx;
static ctap_storage_t storage = CTAP_FILE_STORAGE_CONST_INIT(&empty_ctx);

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

// The core on a loaded store.
static esp_err_t start(void) {
    if (fido_ctap_crypto.init(&fido_ctap_crypto, 0) != CTAP_CRYPTO_OK) {
        return ESP_FAIL;
    }
    ctap_init(&state);
    ready = true;
    ESP_LOGI(TAG, "ready, store %u of %u bytes, up to %u credentials",
             (unsigned)ctx->mem.write_index, (unsigned)ctx->mem.memory_size,
             (unsigned)ctx->max_credentials);
    return ESP_OK;
}

esp_err_t fido_ctap_init(const fido_ctap_hooks_t *h, const char *store_path,
                         size_t max_credentials) {
    if (max_credentials == 0 || ready) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lock == NULL) {
        lock = xSemaphoreCreateMutexStatic(&lock_storage);
    }
    const size_t size = FIDO_CTAP_STORE_BYTES(max_credentials);
    uint8_t *memory = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) {
        memory = heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
    }
    if (memory == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const ctap_file_storage_context_t init = {
        .mem = {.memory_size = size, .memory = memory, .write_index = 0},
        .path = store_path,
        .max_credentials = max_credentials,
        .client = h->client,
    };
    ctap_file_storage_context_t *fresh = heap_caps_malloc(sizeof init, MALLOC_CAP_8BIT);
    if (fresh == NULL) {
        heap_caps_free(memory);
        return ESP_ERR_NO_MEM;
    }
    memcpy(fresh, &init, sizeof init);
    ctx = fresh;
    storage.context = ctx;
    hooks = *h;
    if (store_path != NULL) {
        // The file is encrypted with the vault's data key: fido_ctap_set_key.
        ESP_LOGI(TAG, "locked until the PIN, up to %u credentials", (unsigned)max_credentials);
        return ESP_OK;
    }
    if (storage.init(&storage) != CTAP_STORAGE_OK) {
        return ESP_FAIL;
    }
    return start();
}

const char *fido_ctap_stopped_reason(void) {
    if (ready) {
        return NULL;
    }
    if (ctx->refused != NULL) {
        return ctx->refused;
    }
    return ctx->path != NULL && !ctx->has_key ? "locked: enter the PIN ('pin')" : NULL;
}

// Stops the core and wipes the RAM log and the key (holding the lock).
static void stop(void) {
    ready = false;
    ctap_memory_storage_erase(&storage);
    memset(&ctx->key, 0, sizeof ctx->key);
    ctx->has_key = false;
    memset(&ctx->staged, 0, sizeof ctx->staged);
    ctx->has_staged = false;
    ctx->refused = NULL;
}

esp_err_t fido_ctap_set_key(const fido_ctap_key_t *key) {
    if (lock == NULL || ctx->path == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(lock, portMAX_DELAY);  // a running request finishes first
    esp_err_t err = ESP_OK;
    if (key == NULL) {
        stop();
        ESP_LOGI(TAG, "locked: store and key wiped from memory");
    } else if (!ready) {
        ctx->key = *key;
        ctx->has_key = true;
        ctx->refused = NULL;
        err = storage.init(&storage) == CTAP_STORAGE_OK ? start() : ESP_FAIL;
        if (err != ESP_OK && ctx->refused == NULL) {
            ctx->refused = "the store could not be loaded";
        }
    }
    xSemaphoreGive(lock);
    return err;
}

esp_err_t fido_ctap_stage_key(const fido_ctap_key_t *key) {
    if (lock == NULL || ctx->path == NULL) {
        return ESP_OK;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    struct stat st;
    esp_err_t err = ESP_OK;
    if (ready) {
        ctx->staged = *key;
        ctx->has_staged = true;
        err = ctap_file_storage_stage(&storage) == CTAP_STORAGE_OK ? ESP_OK : ESP_FAIL;
    } else if (stat(ctx->path, &st) == 0) {
        err = ESP_ERR_INVALID_STATE;  // a file not loaded: it cannot be re-keyed
    }
    xSemaphoreGive(lock);
    return err;
}

esp_err_t fido_ctap_commit_key(void) {
    if (lock == NULL || ctx->path == NULL) {
        return ESP_OK;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (ctx->has_staged) {
        err = ctap_file_storage_commit(&storage) == CTAP_STORAGE_OK ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(lock);
    return err;
}

void fido_ctap_unstage_key(void) {
    if (lock == NULL || ctx->path == NULL) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    ctap_file_storage_unstage(&storage);
    xSemaphoreGive(lock);
}

void fido_ctap_wipe(void) {
    if (lock == NULL) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    ctap_file_storage_forget(&storage);
    stop();
    xSemaphoreGive(lock);
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
    xSemaphoreTake(lock, portMAX_DELAY);
    out[0] = ctap_request(&state, msg[0], len - 1, msg + 1, &response);
    xSemaphoreGive(lock);
    return 1 + response.length;
}

// --- administration

static esp_err_t admin_take(void) {
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(lock, pdMS_TO_TICKS(ADMIN_WAIT_MS)) == pdTRUE ? ESP_OK
                                                                         : ESP_ERR_TIMEOUT;
}

static void admin_give(void) { xSemaphoreGive(lock); }

// A stored string (not NUL-terminated) into a C string, cut to fit.
static void copy_text(char *out, const uint8_t *data, size_t size) {
    if (size > FIDO_CTAP_TEXT_SIZE - 1) {
        size = FIDO_CTAP_TEXT_SIZE - 1;
    }
    memcpy(out, data, size);
    out[size] = '\0';
}

// The next valid credential after `item` (start with handle 0).
static bool next_credential(ctap_storage_item_t *item, ctap_credential_handle_t *credential) {
    while (storage.find_item(&storage, item) == CTAP_STORAGE_OK) {
        if (item->size < sizeof(ctap_credential_source_t)) {
            continue;
        }
        credential->source = (const ctap_credential_source_t *)item->data;
        credential->item_handle = item->handle;
        return true;
    }
    return false;
}

esp_err_t fido_ctap_info(fido_ctap_info_t *out) {
    const esp_err_t err = admin_take();
    if (err != ESP_OK) {
        return err;
    }
    memset(out, 0, sizeof *out);
    out->store_used = ctx->mem.write_index;
    out->store_size = ctx->mem.memory_size;
    out->max_credentials = ctx->max_credentials;
    out->persistent = ctx->path != NULL;
    out->encrypted = ctx->path != NULL && ctx->has_key;
    struct stat st;
    if (ctx->path != NULL && stat(ctx->path, &st) == 0) {
        out->file_size = (size_t)st.st_size;
    }
    ctap_storage_item_t item = {.handle = 0, .key = CTAP_STORAGE_KEY_CREDENTIAL};
    ctap_credential_handle_t credential;
    while (next_credential(&item, &credential)) {
        ++out->credentials;
        if (ctap_credential_is_discoverable(&credential)) {
            ++out->discoverable;
        }
    }
    out->pin_set = ctap_is_pin_set(&state);
    out->pin_retries = ctap_get_pin_total_remaining_attempts(&state);
    out->sign_count = ctap_file_storage_counter();
    out->file_version = ctx->version;
    out->kept_version = ctap_file_storage_nvs_version();
    out->modified_utc = ctx->modified_utc;
    admin_give();
    return ESP_OK;
}

esp_err_t fido_ctap_list(fido_ctap_credential_t *out, size_t capacity, size_t *count) {
    *count = 0;
    const esp_err_t err = admin_take();
    if (err != ESP_OK) {
        return err;
    }
    ctap_storage_item_t item = {.handle = 0, .key = CTAP_STORAGE_KEY_CREDENTIAL};
    ctap_credential_handle_t credential;
    while (*count < capacity && next_credential(&item, &credential)) {
        fido_ctap_credential_t *c = &out[(*count)++];
        memset(c, 0, sizeof *c);
        const ctap_credential_source_t *source = credential.source;
        memcpy(c->id, source->id, sizeof c->id);
        ctap_string_t rp_id;
        ctap_credential_get_rp_id(&credential, &rp_id);
        copy_text(c->rp_id, rp_id.data, rp_id.size);
        CTAP_userEntity user;
        ctap_credential_get_user(&credential, &user);
        if (ctap_param_is_present(&user, CTAP_userEntity_name)) {
            copy_text(c->user_name, user.name.data, user.name.size);
        }
        if (ctap_param_is_present(&user, CTAP_userEntity_displayName)) {
            copy_text(c->display_name, user.displayName.data, user.displayName.size);
        }
        c->rp_id_truncated =
            (source->flags & CTAP_CREDENTIAL_SOURCE_CTAP_flags_truncated_rp_id) != 0;
        c->discoverable = ctap_credential_is_discoverable(&credential);
        c->cred_protect = (uint8_t)(source->flags & CTAP_CREDENTIAL_SOURCE_CTAP_flags_credProtect);
        c->size = item.size;
        c->origin = ctap_file_storage_origin(&storage, source->id);
    }
    admin_give();
    return ESP_OK;
}

esp_err_t fido_ctap_delete(const uint8_t id[FIDO_CTAP_CRED_ID_SIZE]) {
    esp_err_t err = admin_take();
    if (err != ESP_OK) {
        return err;
    }
    err = ESP_ERR_NOT_FOUND;
    ctap_storage_item_t item = {.handle = 0, .key = CTAP_STORAGE_KEY_CREDENTIAL};
    ctap_credential_handle_t credential;
    while (next_credential(&item, &credential)) {
        if (memcmp(credential.source->id, id, FIDO_CTAP_CRED_ID_SIZE) == 0) {
            err = ctap_delete_credential(&storage, &credential) == CTAP2_OK ? ESP_OK : ESP_FAIL;
            break;
        }
    }
    admin_give();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "credential deleted from the console");
    }
    return err;
}

// A refused store file: an empty store over it, then the core.
static esp_err_t reset_refused(void) {
    if (storage.erase(&storage) != CTAP_STORAGE_OK) {
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "reset from the console: the refused store file replaced by an empty one");
    return start();
}

esp_err_t fido_ctap_reset(void) {
    if (!ready && ctx->refused != NULL && ctx->has_key) {
        return reset_refused();
    }
    esp_err_t err = admin_take();
    if (err != ESP_OK) {
        return err;
    }
    // What authenticatorReset does after its checks: wipe the store (PIN
    // included) and re-initialise the core's state.
    err = storage.erase(&storage) == CTAP_STORAGE_OK ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        ctap_init(&state);
    }
    admin_give();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "reset from the console: credentials and PIN deleted");
    }
    return err;
}
