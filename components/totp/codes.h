#pragma once

// Codes for a Base32 secret at the previous, current and next window, from the
// crypto driver's TOTP (HMAC-SHA1, 30 s, 6 digits). The key is wiped after use.

#include <cstdint>

#include "esp_err.h"
#include "totp.h"

namespace totp {

struct Codes {
    Windows windows;
    uint32_t code[3];  // previous, current, next
};

// ESP_ERR_INVALID_ARG: not Base32 (or longer than kMaxKeyBytes).
esp_err_t Generate(const char *base32_secret, int64_t now, Codes *out);

}  // namespace totp
