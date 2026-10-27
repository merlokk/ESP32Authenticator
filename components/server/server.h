#pragma once

// The authentication server (server_logic.h): the device's ECDSA P-256 key and
// TOTP secret in `<dir>/server.bin` (SPIFFS, plain, as seed.txt), the
// registration over HTTPS (the certificate bundle) and the TOTP code the user
// signs in with. Requests run in a task of their own, one at a time.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "server_logic.h"

namespace server {

// Loads `<dir>/server.bin` (none: not registered). Once, at boot.
esp_err_t Init(const char *dir);

struct Status {
    bool has_key;
    bool registered;
    int64_t registered_at;  // 0: never
    int64_t last_sync;      // 0: never
    Link link;
    bool busy;               // a request under way
    int http_status;         // the last answer's, 0: none
    char said[48];           // the last request's outcome
};
Status Get();

// The code for the server now (6 digits, 30 s); false: not registered.
bool Code(int64_t unix_seconds, uint32_t *code);

// A new key and TOTP secret (the old ones gone: the device is not registered
// from here), saved, then https://<host>/register?token=... in the task.
// ESP_ERR_INVALID_STATE: a request is under way; ESP_ERR_INVALID_ARG: no host
// or e-mail. The caller sees the outcome in Get().
esp_err_t StartRegister(const char *host, const char *email);

// https://<host>/ asked in the task: any answer is "connected" (Link::kOk),
// and for a registered device the last sync. ESP_ERR_INVALID_STATE: busy.
esp_err_t StartCheck(const char *host);

// The public key, base64url (kPublicSize bytes); false: no key.
bool PublicKey(char *out, size_t size);

}  // namespace server
