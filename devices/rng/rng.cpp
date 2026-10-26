#include "rng.h"

#include "esp_random.h"

namespace rng {

void Fill(uint8_t *out, size_t len) { esp_fill_random(out, len); }

esp_err_t Hex(size_t chars, char *out, size_t size) {
    if (out == nullptr || size <= chars) {
        return ESP_ERR_INVALID_SIZE;
    }
    // A chunk at a time: a small buffer on the stack, overwritten after.
    uint8_t chunk[32];
    constexpr size_t kChunkChars = 2 * sizeof chunk;
    for (size_t at = 0; at < chars; at += kChunkChars) {
        const size_t n = chars - at < kChunkChars ? chars - at : kChunkChars;
        Fill(chunk, BytesForHex(n));
        ToHex(chunk, n, out + at, size - at);
    }
    out[chars] = '\0';
    Fill(chunk, sizeof chunk);
    return ESP_OK;
}

}  // namespace rng
