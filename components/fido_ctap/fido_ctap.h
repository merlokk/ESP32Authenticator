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

#ifdef __cplusplus
}
#endif
