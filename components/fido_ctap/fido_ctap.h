#pragma once

// CTAP 2.1 authenticator core: LionKey (lib/lionkey, unmodified) on ESP-IDF.
// Transport-agnostic: a transport passes whole CTAP messages in and sends the
// response back; keepalive and cancel go through hooks.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called from the task that runs fido_ctap_request().
typedef struct {
    // Processing status for the host: 1 = processing, 2 = user presence needed.
    void (*keepalive)(uint8_t status);
    // True when the host cancelled the running request.
    bool (*cancelled)(void);
    // Level of the user presence input (button held down).
    bool (*button_pressed)(void);
} fido_ctap_hooks_t;

// Loads the credential store from `store_path` (NULL: RAM only, lost on
// reboot) and starts the core. The reset window (10 s) starts here.
esp_err_t fido_ctap_init(const fido_ctap_hooks_t *hooks, const char *store_path);

// One CTAP message: `msg` = command byte + CBOR parameters; `out` = status
// byte + CBOR response. Returns the response length. Blocks while waiting for
// user presence (up to 30 s): run it outside the BLE host task.
size_t fido_ctap_request(const uint8_t *msg, size_t len, uint8_t *out, size_t out_size);

// --- Administration (console). Each call waits up to 1 s for a running
// request (which may be waiting for the button) and fails with
// ESP_ERR_TIMEOUT otherwise.

#define FIDO_CTAP_CRED_ID_SIZE 32
#define FIDO_CTAP_TEXT_SIZE 65  // a stored string (64 bytes max) + NUL

typedef struct {
    size_t store_used;      // bytes in the RAM log (deleted items freed at boot)
    size_t store_size;      // capacity of the RAM log
    size_t file_size;       // the store file on SPIFFS, 0 if none
    size_t credentials;     // stored credentials
    size_t discoverable;    // of which discoverable (resident keys)
    size_t max_credentials;
    bool pin_set;
    uint8_t pin_retries;    // remaining PIN attempts
    uint32_t sign_count;    // global signature counter (NVS, survives reset)
    bool persistent;        // false: RAM only, lost on reboot
} fido_ctap_info_t;

typedef struct {
    uint8_t id[FIDO_CTAP_CRED_ID_SIZE];     // credential ID
    char rp_id[FIDO_CTAP_TEXT_SIZE];        // relying party (site), may be truncated
    char user_name[FIDO_CTAP_TEXT_SIZE];    // "" when not stored
    char display_name[FIDO_CTAP_TEXT_SIZE];
    bool rp_id_truncated;
    bool discoverable;
    uint8_t cred_protect;  // 1..3 (credProtect extension), 0 if unset
    size_t size;           // bytes in the store
} fido_ctap_credential_t;

esp_err_t fido_ctap_info(fido_ctap_info_t *out);

// Fills up to `capacity` credentials in store order; `*count` = how many.
esp_err_t fido_ctap_list(fido_ctap_credential_t *out, size_t capacity, size_t *count);

// Deletes the credential with this ID. ESP_ERR_NOT_FOUND if none.
esp_err_t fido_ctap_delete(const uint8_t id[FIDO_CTAP_CRED_ID_SIZE]);

// Deletes every credential and the PIN, as authenticatorReset does (but
// without its 10 s / button rule: the console is the authority). The
// signature counter is kept.
esp_err_t fido_ctap_reset(void);

#ifdef __cplusplus
}
#endif
