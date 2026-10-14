#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

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

// AES-256-CBC without padding, in place; `len` is a multiple of 16.
esp_err_t Aes256CbcEncrypt(const uint8_t key[kAes256KeySize], const uint8_t iv[kAesBlockSize],
                           uint8_t *data, size_t len);
esp_err_t Aes256CbcDecrypt(const uint8_t key[kAes256KeySize], const uint8_t iv[kAesBlockSize],
                           uint8_t *data, size_t len);

}  // namespace crypto
