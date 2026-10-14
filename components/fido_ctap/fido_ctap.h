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

// The host that sent the request: kept with every credential it creates.
typedef enum {
    FIDO_CTAP_TRANSPORT_NONE = 0,
    FIDO_CTAP_TRANSPORT_BLE = 1,
    FIDO_CTAP_TRANSPORT_USB = 2,
} fido_ctap_transport_t;

typedef struct {
    uint8_t transport;  // fido_ctap_transport_t
    uint8_t addr_type;  // BLE: 0 public, 1 random (static); identity address
    uint8_t addr[6];    // BLE: most significant byte first, as printed
} fido_ctap_client_t;

// Where and when a credential was made: for the sync between several
// authenticators (which one created a record) and for the console.
typedef struct {
    fido_ctap_client_t client;  // the host; transport NONE when unknown
    int64_t created_utc;        // Unix seconds; 0: no believable time then
    uint8_t device[6];          // this authenticator's base MAC (eFuse)
    bool has_device;            // false: made before this was recorded
} fido_ctap_origin_t;

// Called from the task that runs fido_ctap_request().
typedef struct {
    // Processing status for the host: 1 = processing, 2 = user presence needed.
    void (*keepalive)(uint8_t status);
    // True when the host cancelled the running request.
    bool (*cancelled)(void);
    // Level of the user presence input (button held down).
    bool (*button_pressed)(void);
    // The current host; false (or NULL hook) when unknown.
    bool (*client)(fido_ctap_client_t *out);
} fido_ctap_hooks_t;

// RAM log for `n` credentials (~635 bytes each at most, rounded up), plus the
// PIN state and room for items replaced during this boot (dropped at the next).
#define FIDO_CTAP_STORE_BYTES(n) ((size_t)(n) * 1024 + 2048)

// Prepares the credential store `store_path` (NULL: RAM only, lost on reboot,
// started at once). A file store starts with fido_ctap_set_key(): it is
// encrypted with the vault's data key, so FIDO2 is off until the PIN. Up to
// `max_credentials` (the board's fido_max_credentials; the 1+th gets
// KEY_STORE_FULL). A store file that is refused (older than the version kept
// in NVS, missing while one is kept, damaged, plain after an encrypted one,
// larger than the RAM log) is not touched: FIDO2 stays off
// (fido_ctap_stopped_reason). The reset window (10 s) starts with the core.
esp_err_t fido_ctap_init(const fido_ctap_hooks_t *hooks, const char *store_path,
                         size_t max_credentials);

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
    bool encrypted;         // the file is AES-256-GCM with the vault's PIN key
    uint32_t file_version;  // the store file's version (+1 per write), 0 if none yet
    uint32_t kept_version;  // highest version this device accepted (NVS)
    int64_t modified_utc;   // last change, Unix seconds; 0 unknown
} fido_ctap_info_t;

// The store key from the vault: PBKDF2(PIN, salt), and what goes into the
// file header so another device with the PIN derives the same key.
typedef struct {
    uint8_t key[32];
    uint8_t salt[16];
    uint32_t iterations;
    uint8_t id[8];  // a hash of the key (store header)
} fido_ctap_key_t;

// Why the core is not running (the store file was refused: rollback, damaged,
// too large), NULL if it runs or init was not called.
const char *fido_ctap_stopped_reason(void);

// The store key from the vault: loads the store and starts the core. NULL:
// stops the core, wiping the store and the key from memory. Waits for a
// running request.
esp_err_t fido_ctap_set_key(const fido_ctap_key_t *key);

// A PIN change (store_io.h: several files replaced together): stage writes
// `<file>.new` with the new key from memory (nothing to do without a file and
// a running core: ESP_OK; a file that is not loaded: ESP_ERR_INVALID_STATE);
// commit renames it over the file and keeps the new key; unstage drops it.
esp_err_t fido_ctap_stage_key(const fido_ctap_key_t *key);
esp_err_t fido_ctap_commit_key(void);
void fido_ctap_unstage_key(void);

// The wipe PIN: the store file and its kept version gone, the core stopped.
// Starts empty with the next key.
void fido_ctap_wipe(void);

typedef struct {
    uint8_t id[FIDO_CTAP_CRED_ID_SIZE];     // credential ID
    char rp_id[FIDO_CTAP_TEXT_SIZE];        // relying party (site), may be truncated
    char user_name[FIDO_CTAP_TEXT_SIZE];    // "" when not stored
    char display_name[FIDO_CTAP_TEXT_SIZE];
    bool rp_id_truncated;
    bool discoverable;
    uint8_t cred_protect;  // 1..3 (credProtect extension), 0 if unset
    size_t size;           // bytes in the store
    // Who, where and when; all unknown for credentials made before this was kept.
    fido_ctap_origin_t origin;
} fido_ctap_credential_t;

esp_err_t fido_ctap_info(fido_ctap_info_t *out);

// Fills up to `capacity` credentials in store order; `*count` = how many.
esp_err_t fido_ctap_list(fido_ctap_credential_t *out, size_t capacity, size_t *count);

// Deletes the credential with this ID. ESP_ERR_NOT_FOUND if none.
esp_err_t fido_ctap_delete(const uint8_t id[FIDO_CTAP_CRED_ID_SIZE]);

// Deletes every credential and the PIN, as authenticatorReset does (but
// without its 10 s / button rule: the console is the authority). The
// signature counter is kept. Also works when the store file was refused
// (fido_ctap_stopped_reason): writes an empty store over it and starts the core.
esp_err_t fido_ctap_reset(void);

#ifdef __cplusplus
}
#endif
