#pragma once

// PIN complexity policy (host-tested, C and C++): what a new PIN must not be,
// beyond its length. Shared by the FIDO2 clientPIN (CTAP pinComplexityPolicy)
// and the vault PIN. Bytes of the PIN as given (UTF-8 for CTAP); letters
// compare without case. Only for a PIN being set: an existing one stays.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_POLICY_OK = 0,
    PIN_POLICY_REPEATED,  // one block over and over: 1111, 1212, 123123
    PIN_POLICY_RUN,       // a run up or down: 1234, 654321, abcd, 7890; also doubled: 112233
    PIN_POLICY_COMMON,    // on the list of common PINs
} pin_policy_result_t;

static inline uint8_t pin_policy_lower(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c - 'A' + 'a') : c;
}

static inline bool pin_policy_is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

static inline bool pin_policy_is_letter(uint8_t c) {
    c = pin_policy_lower(c);
    return c >= 'a' && c <= 'z';
}

// The whole PIN is `block` repeated (block shorter than the PIN).
static inline bool pin_policy_repeated(const uint8_t *pin, size_t len) {
    for (size_t block = 1; block <= len / 2; ++block) {
        if (len % block != 0) {
            continue;
        }
        bool same = true;
        for (size_t i = block; i < len && same; ++i) {
            same = pin_policy_lower(pin[i]) == pin_policy_lower(pin[i % block]);
        }
        if (same) {
            return true;
        }
    }
    return false;
}

// Every `stride`-th character from `start` steps by +1 or by -1 (digits
// wrap: 7890), at least 3 of them, all digits or all letters.
static inline bool pin_policy_run(const uint8_t *pin, size_t len, size_t start, size_t stride) {
    const size_t n = (len - start + stride - 1) / stride;
    if (n < 3) {
        return false;
    }
    const uint8_t first = pin_policy_lower(pin[start]);
    const bool digits = pin_policy_is_digit(first);
    if (!digits && !pin_policy_is_letter(first)) {
        return false;
    }
    int step = 0;
    for (size_t k = 1; k < n; ++k) {
        const uint8_t a = pin_policy_lower(pin[start + (k - 1) * stride]);
        const uint8_t b = pin_policy_lower(pin[start + k * stride]);
        if (digits ? !pin_policy_is_digit(b) : !pin_policy_is_letter(b)) {
            return false;
        }
        int d = (int)b - (int)a;
        if (digits) {
            d = (d + 10) % 10;  // 0..9; +1 -> 1, -1 -> 9
            d = d == 9 ? -1 : d;
        }
        if (d != 1 && d != -1) {
            return false;
        }
        if (step != 0 && d != step) {
            return false;
        }
        step = d;
    }
    return true;
}

static inline bool pin_policy_common(const uint8_t *pin, size_t len) {
    // Beyond what the repeat and run rules catch; at least 4 characters each.
    static const char *const kCommon[] = {
        "1004",   "2000",     "2001",     "2580",     "0852",     "1q2w",     "147258",
        "147852", "159753",   "258369",   "741852",   "789456",   "963852",   "123321",
        "102030", "1q2w3e",   "1qaz2wsx", "abc123",   "admin",    "asdf",     "asdfgh",
        "azerty", "dragon",   "iloveyou", "letmein",  "monkey",   "passw0rd", "password",
        "qwerty", "qwerty123", "qwertz",  "welcome",  "zxcvbn",
    };
    for (size_t i = 0; i < sizeof kCommon / sizeof kCommon[0]; ++i) {
        const char *word = kCommon[i];
        size_t j = 0;
        while (j < len && word[j] != '\0' && pin_policy_lower(pin[j]) == (uint8_t)word[j]) {
            ++j;
        }
        if (j == len && word[j] == '\0') {
            return true;
        }
    }
    return false;
}

static inline pin_policy_result_t pin_policy_check(const uint8_t *pin, size_t len) {
    if (len >= 2 && pin_policy_repeated(pin, len)) {
        return PIN_POLICY_REPEATED;
    }
    if (pin_policy_run(pin, len, 0, 1)) {
        return PIN_POLICY_RUN;
    }
    // doubled run: 112233, aabbcc
    if (len >= 6 && len % 2 == 0) {
        bool pairs = true;
        for (size_t i = 0; i < len && pairs; i += 2) {
            pairs = pin_policy_lower(pin[i]) == pin_policy_lower(pin[i + 1]);
        }
        if (pairs && pin_policy_run(pin, len, 0, 2)) {
            return PIN_POLICY_RUN;
        }
    }
    if (pin_policy_common(pin, len)) {
        return PIN_POLICY_COMMON;
    }
    return PIN_POLICY_OK;
}

static inline const char *pin_policy_describe(pin_policy_result_t result) {
    switch (result) {
        case PIN_POLICY_OK:
            return "ok";
        case PIN_POLICY_REPEATED:
            return "repeats itself";
        case PIN_POLICY_RUN:
            return "a run (like 1234 or 9876)";
        case PIN_POLICY_COMMON:
            return "a common PIN";
    }
    return "?";
}

#ifdef __cplusplus
}
#endif
