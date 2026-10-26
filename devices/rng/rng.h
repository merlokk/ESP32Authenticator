#pragma once

// The ESP32-S3 hardware random number generator (esp_fill_random). True
// random while the radio (Wi-Fi or BLE) is on: its RF noise feeds it; with
// the radio off ESP-IDF does not promise true randomness (its hardware RNG
// guide). Any task, no setup.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "rng_logic.h"

namespace rng {

// `len` random bytes.
void Fill(uint8_t *out, size_t len);

// `chars` random lowercase hex digits and a NUL (rng_logic.h ToHex).
// ESP_ERR_INVALID_SIZE: `size` has no room for them.
esp_err_t Hex(size_t chars, char *out, size_t size);

}  // namespace rng
