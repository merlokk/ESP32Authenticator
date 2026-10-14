#pragma once

// The pure half of HOTP (RFC 4226) / TOTP (RFC 6238): the time step and the
// dynamic truncation of an HMAC-SHA1. Host-tested; the HMAC is crypto.h.

#include <cstddef>
#include <cstdint>

namespace crypto {

inline constexpr size_t kSha1Size = 20;
inline constexpr uint32_t kTotpStep = 30;
inline constexpr unsigned kOtpDigits = 6;
inline constexpr unsigned kOtpMaxDigits = 8;

// T = floor(t / step); 0 before the epoch.
constexpr uint64_t OtpCounter(int64_t unix_seconds, uint32_t step = kTotpStep) {
    return unix_seconds < 0 || step == 0 ? 0 : static_cast<uint64_t>(unix_seconds) / step;
}

// The HOTP message: the counter, 8 bytes big-endian.
constexpr void OtpMessage(uint64_t counter, uint8_t out[8]) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<uint8_t>(counter);
        counter >>= 8;
    }
}

// Dynamic truncation (RFC 4226 5.3) to `digits` (1..8) decimal digits.
constexpr uint32_t OtpTruncate(const uint8_t mac[kSha1Size], unsigned digits = kOtpDigits) {
    const unsigned offset = mac[kSha1Size - 1] & 0x0F;
    const uint32_t value = (static_cast<uint32_t>(mac[offset] & 0x7F) << 24) |
                           (static_cast<uint32_t>(mac[offset + 1]) << 16) |
                           (static_cast<uint32_t>(mac[offset + 2]) << 8) | mac[offset + 3];
    uint32_t modulo = 1;
    for (unsigned i = 0; i < digits && i < kOtpMaxDigits; ++i) {
        modulo *= 10;
    }
    return value % modulo;
}

}  // namespace crypto
