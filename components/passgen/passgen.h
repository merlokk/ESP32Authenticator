#pragma once

// The password generator (as merlokk.com/password-generator): words drawn
// uniformly from a list (rejection sampling, no modulo bias), or only their
// distinct 3-letter starts; a separator from kSeparators between them; the
// first letters capitalised; a digit and a symbol, each after a random word.
// Pure (host-tested): the caller gives the random numbers (the hardware RNG)
// and the list (passgen_words.h).

#include <cstddef>
#include <cstdint>

namespace passgen {

enum class List : uint8_t {
    kEff,     // EFF large, 7772 words
    kBip39,   // BIP-39 English, 2048
    kSlip39,  // SLIP-39, 1024
};
inline constexpr uint8_t kListCount = 3;
inline constexpr const char *kListNames[kListCount] = {"eff", "bip39", "slip39"};  // config.json
inline constexpr const char *kListLabels[kListCount] = {"EFF", "BIP-39", "SLIP-39"};

// The separators to choose from: config.json's text, the label shown.
struct Separator {
    const char *text;
    const char *label;
};
inline constexpr Separator kSeparators[] = {
    {"-", "-"}, {".", "."}, {"_", "_"}, {" ", "space"}, {"", "none"}};
inline constexpr uint8_t kSeparatorCount = sizeof kSeparators / sizeof kSeparators[0];

inline constexpr uint8_t kMinWords = 3;
inline constexpr uint8_t kMaxWords = 12;
inline constexpr size_t kMaxWord = 9;  // the longest word of the lists (EFF)
// The words, a separator after each but the last, a digit, a symbol, NUL.
inline constexpr size_t kTextSize = kMaxWords * (kMaxWord + 1) + 2 + 1;
inline constexpr char kSymbols[] = "!@#$%&*?+=";
inline constexpr uint8_t kSymbolCount = sizeof kSymbols - 1;

struct Options {
    uint8_t words = 5;      // kMinWords..kMaxWords
    List list = List::kEff;
    uint8_t separator = 0;  // kSeparators' index
    bool prefix = false;    // the first 3 letters of each word only
    bool capitalize = false;
    bool digit = false;
    bool symbol = false;
};

constexpr bool Equal(const Options &a, const Options &b) {
    return a.words == b.words && a.list == b.list && a.separator == b.separator &&
           a.prefix == b.prefix && a.capitalize == b.capitalize && a.digit == b.digit &&
           a.symbol == b.symbol;
}

// The index of a list's / separator's config.json text, -1 if none.
constexpr int FindList(const char *name) {
    for (uint8_t i = 0; i < kListCount; ++i) {
        const char *a = kListNames[i];
        const char *b = name;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') {
            return i;
        }
    }
    return -1;
}
constexpr int FindSeparator(const char *text) {
    for (uint8_t i = 0; i < kSeparatorCount; ++i) {
        const char *a = kSeparators[i].text;
        const char *b = text;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') {
            return i;
        }
    }
    return -1;
}

// A word list: sorted, so words with the same 3-letter start are together.
struct Words {
    const char *const *word;
    size_t count;
};

// A 32-bit random number (uniform).
using Random = uint32_t (*)(void *ctx);

// Uniform in [0, n): numbers past the last whole multiple of n are drawn again.
inline uint32_t Uniform(uint32_t n, Random random, void *ctx) {
    if (n <= 1) {
        return 0;
    }
    const uint64_t limit = ((uint64_t{1} << 32) / n) * n;
    uint32_t r = 0;
    do {
        r = random(ctx);
    } while (r >= limit);
    return r % n;
}

constexpr size_t Length(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

// A word's start: its first 3 letters (a shorter word whole).
constexpr size_t StartLength(const char *w) { return Length(w) < 3 ? Length(w) : 3; }

constexpr bool SameStart(const char *a, const char *b) {
    const size_t n = StartLength(a);
    if (n != StartLength(b)) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// What a word is drawn from: the words, or their distinct starts.
constexpr size_t PoolSize(const Words &w, bool prefix) {
    if (!prefix) {
        return w.count;
    }
    size_t n = 0;
    for (size_t i = 0; i < w.count; ++i) {
        if (i == 0 || !SameStart(w.word[i - 1], w.word[i])) {
            ++n;
        }
    }
    return n;
}

// The pool's item `k` (< PoolSize): its text and length.
constexpr const char *PoolItem(const Words &w, bool prefix, size_t k, size_t *length) {
    if (!prefix) {
        *length = Length(w.word[k]);
        return w.word[k];
    }
    size_t n = 0;
    for (size_t i = 0; i < w.count; ++i) {
        if (i == 0 || !SameStart(w.word[i - 1], w.word[i])) {
            if (n++ == k) {
                *length = StartLength(w.word[i]);
                return w.word[i];
            }
        }
    }
    *length = 0;
    return "";
}

// A password into `out` (kTextSize); its length. `pool`: PoolSize's (0: counted here).
inline size_t Generate(const Options &o, const Words &w, Random random, void *ctx,
                       char out[kTextSize], size_t pool = 0) {
    const uint8_t n = o.words < kMinWords ? kMinWords : o.words > kMaxWords ? kMaxWords : o.words;
    pool = pool != 0 ? pool : PoolSize(w, o.prefix);
    uint32_t pick[kMaxWords] = {};
    for (uint8_t i = 0; i < n; ++i) {
        pick[i] = Uniform(static_cast<uint32_t>(pool), random, ctx);
    }
    int digit_at = -1;
    char digit = '0';
    if (o.digit) {
        digit_at = static_cast<int>(Uniform(n, random, ctx));
        digit = static_cast<char>('0' + Uniform(10, random, ctx));
    }
    int symbol_at = -1;
    char symbol = kSymbols[0];
    if (o.symbol) {
        symbol_at = static_cast<int>(Uniform(n, random, ctx));
        symbol = kSymbols[Uniform(kSymbolCount, random, ctx)];
    }
    const char *sep =
        o.separator < kSeparatorCount ? kSeparators[o.separator].text : kSeparators[0].text;
    size_t used = 0;
    const auto put = [&](char c) {
        if (used + 1 < kTextSize) {
            out[used++] = c;
        }
    };
    for (uint8_t i = 0; i < n; ++i) {
        if (i > 0) {
            for (const char *s = sep; *s != '\0'; ++s) {
                put(*s);
            }
        }
        size_t len = 0;
        const char *word = PoolItem(w, o.prefix, pick[i], &len);
        for (size_t j = 0; j < len; ++j) {
            const char c = word[j];
            put(j == 0 && o.capitalize && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A')
                                                                 : c);
        }
        if (digit_at == i) {
            put(digit);
        }
        if (symbol_at == i) {
            put(symbol);
        }
    }
    out[used] = '\0';
    return used;
}

// log2(x) in thousandths (x >= 1), to within one.
constexpr uint32_t Log2Milli(uint32_t x) {
    if (x <= 1) {
        return 0;
    }
    uint32_t k = 0;
    while ((x >> (k + 1)) != 0) {
        ++k;
    }
    // x / 2^k in [1, 2), 28 fraction bits; each squaring gives a bit.
    constexpr uint32_t kQ = 28;
    uint64_t y = (uint64_t{x} << kQ) >> k;
    uint32_t frac = 0;
    for (int i = 0; i < 20; ++i) {
        y = (y * y) >> kQ;
        frac <<= 1;
        if (y >= (uint64_t{2} << kQ)) {
            y >>= 1;
            frac |= 1;
        }
    }
    return k * 1000 + static_cast<uint32_t>((uint64_t{frac} * 1000 + (1u << 19)) >> 20);
}

// The strength in thousandths of a bit: the words, where the digit / symbol
// goes and which one (capitals and the separator are fixed: nothing).
constexpr uint32_t MilliBits(const Options &o, size_t pool) {
    const uint32_t n = o.words;
    uint32_t b = n * Log2Milli(static_cast<uint32_t>(pool));
    if (o.digit) {
        b += Log2Milli(10) + Log2Milli(n);
    }
    if (o.symbol) {
        b += Log2Milli(kSymbolCount) + Log2Milli(n);
    }
    return b;
}

constexpr const char *StrengthLabel(uint32_t milli_bits) {
    return milli_bits < 45000   ? "Weak"
           : milli_bits < 60000 ? "Fair"
           : milli_bits < 80000 ? "Strong"
                                : "Very strong";
}

}  // namespace passgen
