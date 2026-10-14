#pragma once

// TOTP codes for a secret as authenticator apps show it: Base32 decoding and the
// previous / current / next time windows. Pure (host-tested); the algorithm
// itself (HMAC-SHA1, truncation) is in devices/crypto, used by codes.h.

#include <cstddef>
#include <cstdint>

namespace totp {

inline constexpr size_t kMaxKeyBytes = 64;
inline constexpr uint32_t kStep = 30;
inline constexpr unsigned kDigits = 6;

// Base32 (A-Z, 2-7), case-insensitive; spaces, '-' and '=' padding ignored.
// Returns the byte count; 0 on a bad character, empty input or overflow.
size_t Base32Decode(const char *text, uint8_t *out, size_t capacity);

// Base64, standard (+/) or URL-safe (-_); spaces and '=' padding ignored.
// Returns the byte count; 0 on a bad character, a stray last digit, empty
// input or overflow.
size_t Base64Decode(const char *text, uint8_t *out, size_t capacity);

// Upper-case Base32 without padding; false if `capacity` (with the NUL) is
// too small.
bool Base32Encode(const uint8_t *data, size_t size, char *out, size_t capacity);

// The window `now` falls in, and its neighbours.
struct Windows {
    int64_t start;         // Unix seconds the current window began
    uint32_t remaining;    // seconds until the next one, 1..step
    int64_t times[3];      // a moment in the previous, current and next window
};

Windows WindowsAt(int64_t now, uint32_t step = kStep);

}  // namespace totp
