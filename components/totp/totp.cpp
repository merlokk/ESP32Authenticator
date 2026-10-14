#include "totp.h"

namespace totp {

size_t Base32Decode(const char *text, uint8_t *out, size_t capacity) {
    if (text == nullptr || out == nullptr) {
        return 0;
    }
    uint32_t buffer = 0;
    int bits = 0;
    size_t length = 0;
    for (const char *p = text; *p != '\0'; ++p) {
        const char c = *p;
        if (c == ' ' || c == '-' || c == '=') {
            continue;
        }
        int value = -1;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a';
        } else if (c >= '2' && c <= '7') {
            value = c - '2' + 26;
        }
        if (value < 0) {
            return 0;
        }
        buffer = (buffer << 5) | static_cast<uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (length == capacity) {
                return 0;
            }
            out[length++] = static_cast<uint8_t>(buffer >> bits);
        }
    }
    return length;  // leftover bits (< 8) are padding
}

size_t Base64Decode(const char *text, uint8_t *out, size_t capacity) {
    if (text == nullptr || out == nullptr) {
        return 0;
    }
    uint32_t buffer = 0;
    int bits = 0;
    size_t digits = 0;
    size_t length = 0;
    for (const char *p = text; *p != '\0'; ++p) {
        const char c = *p;
        if (c == ' ' || c == '=') {
            continue;
        }
        int value = -1;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            value = c - '0' + 52;
        } else if (c == '+' || c == '-') {
            value = 62;
        } else if (c == '/' || c == '_') {
            value = 63;
        }
        if (value < 0) {
            return 0;
        }
        ++digits;
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (length == capacity) {
                return 0;
            }
            out[length++] = static_cast<uint8_t>(buffer >> bits);
        }
    }
    // One digit over a group of four carries no whole byte: not Base64.
    return digits % 4 == 1 ? 0 : length;
}

bool Base32Encode(const uint8_t *data, size_t size, char *out, size_t capacity) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    const size_t need = (size * 8 + 4) / 5;
    if (out == nullptr || capacity < need + 1 || (data == nullptr && size != 0)) {
        return false;
    }
    uint32_t buffer = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < size; ++i) {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out[n++] = kAlphabet[(buffer >> bits) & 31];
        }
    }
    if (bits > 0) {
        out[n++] = kAlphabet[(buffer << (5 - bits)) & 31];
    }
    out[n] = '\0';
    return true;
}

Windows WindowsAt(int64_t now, uint32_t step) {
    Windows w = {};
    if (step == 0) {
        step = kStep;
    }
    const int64_t into = now >= 0 ? now % step : 0;
    w.start = now - into;
    w.remaining = static_cast<uint32_t>(step - into);
    w.times[0] = w.start - step;
    w.times[1] = w.start;
    w.times[2] = w.start + step;
    return w;
}

}  // namespace totp
