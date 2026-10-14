#include "codes.h"

#include <cstring>

#include "crypto.h"

namespace totp {

esp_err_t Generate(const char *base32_secret, int64_t now, Codes *out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // Static, not on the (console) task stack; callers are serialised by the console.
    static uint8_t key[kMaxKeyBytes];
    const size_t key_len = Base32Decode(base32_secret, key, sizeof key);
    if (key_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = crypto::Init();
    out->windows = WindowsAt(now);
    for (int i = 0; i < 3 && err == ESP_OK; ++i) {
        err = crypto::Totp(key, key_len, out->windows.times[i], kStep, kDigits, &out->code[i]);
    }
    memset(key, 0, sizeof key);
    return err;
}

}  // namespace totp
