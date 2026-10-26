#pragma once

// The pure half of the random number generator (host-tested): random bytes
// as a hex string.

#include <cstddef>
#include <cstdint>

namespace rng {

inline constexpr size_t kMaxHexChars = 2048;  // the console's limit (1 KB of bytes)

// Bytes that make `chars` hex digits (an odd count: the last byte's high nibble).
constexpr size_t BytesForHex(size_t chars) { return (chars + 1) / 2; }

// `chars` lowercase hex digits of `bytes` (BytesForHex(chars) of them), then
// a NUL; false (nothing written): no room for them in `size`.
constexpr bool ToHex(const uint8_t *bytes, size_t chars, char *out, size_t size) {
    if (size <= chars) {
        return false;
    }
    constexpr char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < chars; ++i) {
        const uint8_t b = bytes[i / 2];
        out[i] = kDigits[i % 2 == 0 ? b >> 4 : b & 0x0F];
    }
    out[chars] = '\0';
    return true;
}

}  // namespace rng
