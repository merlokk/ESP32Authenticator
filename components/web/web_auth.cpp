#include "web_auth.h"

namespace web {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t Length(const char *text) {
    size_t length = 0;
    if (text != nullptr) {
        while (text[length] != '\0') {
            ++length;
        }
    }
    return length;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

// Runs over all of `expected` whatever matches, so the time says nothing.
bool SameSecret(const char *expected, size_t expected_length, const char *given,
                size_t given_length) {
    uint32_t difference = static_cast<uint32_t>(expected_length ^ given_length);
    for (size_t i = 0; i < expected_length; ++i) {
        const char c = (i < given_length) ? given[i] : '\0';
        difference |= static_cast<uint32_t>(static_cast<unsigned char>(expected[i]) ^
                                            static_cast<unsigned char>(c));
    }
    return difference == 0;
}

}  // namespace

bool AuthRequired(const char *user, const char *password) {
    return Length(user) != 0 && Length(password) != 0;
}

bool BasicCredential(const char *user, const char *password, char *out, size_t capacity) {
    if (out == nullptr) {
        return false;
    }
    // ':' separates the two: "a:b" + "c" and "a" + "b:c" would be one credential.
    for (size_t i = 0; user != nullptr && user[i] != '\0'; ++i) {
        if (user[i] == ':') {
            return false;
        }
    }
    const size_t user_length = Length(user);
    const size_t password_length = Length(password);
    const size_t plain_length = user_length + 1 + password_length;
    if (plain_length + 1 > kMaxCredentialSize) {
        return false;
    }
    const size_t encoded_length = ((plain_length + 2) / 3) * 4;
    if (encoded_length + 1 > capacity) {
        return false;
    }
    auto plain = [&](size_t i) -> unsigned char {
        if (i < user_length) {
            return static_cast<unsigned char>(user[i]);
        }
        if (i == user_length) {
            return static_cast<unsigned char>(':');
        }
        return static_cast<unsigned char>(password[i - user_length - 1]);
    };
    size_t written = 0;
    for (size_t i = 0; i < plain_length; i += 3) {
        const unsigned char b0 = plain(i);
        const unsigned char b1 = (i + 1 < plain_length) ? plain(i + 1) : 0;
        const unsigned char b2 = (i + 2 < plain_length) ? plain(i + 2) : 0;
        out[written++] = kAlphabet[b0 >> 2];
        out[written++] = kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
        out[written++] = (i + 1 < plain_length) ? kAlphabet[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=';
        out[written++] = (i + 2 < plain_length) ? kAlphabet[b2 & 0x3F] : '=';
    }
    out[written] = '\0';
    return true;
}

bool Authorised(const char *user, const char *password, const char *header) {
    if (!AuthRequired(user, password)) {
        return true;
    }
    char expected[kMaxEncodedSize] = {};
    if (!BasicCredential(user, password, expected, sizeof expected) || header == nullptr) {
        return false;
    }
    // The scheme token is case-insensitive (RFC 7235); the base64 is data.
    size_t at = 0;
    while (kAuthScheme[at] != '\0') {
        if (Lower(header[at]) != Lower(kAuthScheme[at])) {
            return false;
        }
        ++at;
    }
    if (!IsSpace(header[at])) {
        return false;
    }
    while (IsSpace(header[at])) {
        ++at;
    }
    size_t end = at + Length(header + at);
    while (end > at && IsSpace(header[end - 1])) {
        --end;
    }
    if (end == at) {
        return false;
    }
    return SameSecret(expected, Length(expected), header + at, end - at);
}

}  // namespace web
