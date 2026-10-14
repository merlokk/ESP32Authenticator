#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "otp.h"

// Hardware crypto of the ESP32-S3 through PSA (mbedTLS routes it to the SHA
// and AES accelerators) and the hardware RNG. The S3 has no ECC accelerator.

namespace crypto {

constexpr size_t kSha256Size = 32;
constexpr size_t kAes256KeySize = 32;
constexpr size_t kAesBlockSize = 16;

// Initializes PSA. Safe to call more than once.
esp_err_t Init();

// Random bytes: a true RNG while the radio (BLE/Wi-Fi) is on.
void Random(uint8_t *out, size_t len);

esp_err_t Sha256(const uint8_t *data, size_t len, uint8_t out[kSha256Size]);

esp_err_t HmacSha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                   uint8_t out[kSha1Size]);

// HOTP (RFC 4226) and TOTP (RFC 6238), HMAC-SHA1; `digits` 6..8.
esp_err_t Hotp(const uint8_t *key, size_t key_len, uint64_t counter, unsigned digits,
               uint32_t *code);
esp_err_t Totp(const uint8_t *key, size_t key_len, int64_t unix_seconds, uint32_t step,
               unsigned digits, uint32_t *code);

// The RFC 6238 SHA-1 vectors (8 digits) through Totp(). ESP_OK when all match.
esp_err_t TotpSelfTest();

// AES-256-CBC without padding, in place; `len` is a multiple of 16.
esp_err_t Aes256CbcEncrypt(const uint8_t key[kAes256KeySize], const uint8_t iv[kAesBlockSize],
                           uint8_t *data, size_t len);
esp_err_t Aes256CbcDecrypt(const uint8_t key[kAes256KeySize], const uint8_t iv[kAesBlockSize],
                           uint8_t *data, size_t len);

}  // namespace crypto
