#pragma once

// `nvs ls`: which values it may print. A string whose key names a password,
// a credential or a key shows only its length; blobs show only their size
// (BLE bonds keep their keys in them). Printable ASCII, cut to kMaxShown.
// <cstdint>/<cstddef> only: host-tested.

#include <cstddef>
#include <cstdint>

namespace console::nvs_show {

inline constexpr size_t kMaxShown = 64;

constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// `word` (lower case) anywhere in `text`, any case.
constexpr bool Contains(const char *text, const char *word) {
    for (; *text != '\0'; ++text) {
        size_t i = 0;
        while (word[i] != '\0' && Lower(text[i]) == word[i]) {
            ++i;
        }
        if (word[i] == '\0') {
            return true;
        }
    }
    return false;
}

// The stock firmware's sta_pwd / wifi_creds, ESP-IDF's sta.pswd, anything
// called a key, token, secret or PIN.
constexpr bool SecretKey(const char *key) {
    constexpr const char *kWords[] = {"pwd", "pswd", "pass", "cred", "secret", "token",
                                      "key", "psk",  "pin",  "seed", "ltk",    "irk"};
    for (const char *word : kWords) {
        if (Contains(key, word)) {
            return true;
        }
    }
    return false;
}

// A string as shown: printable ASCII kept, the rest '.', cut to kMaxShown
// ("..." after it). `out` holds kMaxShown + 4 bytes.
constexpr void Printable(const char *text, char *out) {
    size_t n = 0;
    for (; text[n] != '\0' && n < kMaxShown; ++n) {
        const char c = text[n];
        out[n] = c >= 0x20 && c < 0x7F ? c : '.';
    }
    if (text[n] != '\0') {
        out[n++] = '.';
        out[n++] = '.';
        out[n++] = '.';
    }
    out[n] = '\0';
}

}  // namespace console::nvs_show
