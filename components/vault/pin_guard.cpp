#include "pin_guard.h"

namespace vault {

namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseHex(const char *text, uint8_t *out, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        const int hi = HexValue(text[2 * i]);
        const int lo = hi < 0 ? -1 : HexValue(text[2 * i + 1]);
        if (lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

char *PutHex(const uint8_t *data, size_t bytes, char *out) {
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        *out++ = kDigits[data[i] >> 4];
        *out++ = kDigits[data[i] & 0xF];
    }
    return out;
}

}  // namespace

uint32_t LockSecondsAtBoot(uint8_t failures, int64_t deadline_utc, int64_t now_utc,
                           uint32_t lock_seconds) {
    if (failures < kMaxPinTries) {
        return 0;
    }
    if (deadline_utc <= 0 || now_utc <= 0) {
        return lock_seconds;
    }
    const int64_t left = deadline_utc - now_utc;
    if (left <= 0) {
        return 0;
    }
    return left > lock_seconds ? lock_seconds : static_cast<uint32_t>(left);
}

bool ParseWipe(const char *text, uint8_t salt[kWipeSaltSize], uint8_t hash[kWipeHashSize]) {
    for (size_t i = 0; i < kWipeTextSize - 1; ++i) {
        if (text[i] == '\0') {
            return false;
        }
    }
    return text[kWipeTextSize - 1] == '\0' && text[2 * kWipeSaltSize] == ':' &&
           ParseHex(text, salt, kWipeSaltSize) &&
           ParseHex(text + 2 * kWipeSaltSize + 1, hash, kWipeHashSize);
}

void FormatWipe(const uint8_t salt[kWipeSaltSize], const uint8_t hash[kWipeHashSize],
                char out[kWipeTextSize]) {
    char *p = PutHex(salt, kWipeSaltSize, out);
    *p++ = ':';
    p = PutHex(hash, kWipeHashSize, p);
    *p = '\0';
}

}  // namespace vault
