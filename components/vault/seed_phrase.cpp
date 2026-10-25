#include "seed_phrase.h"

namespace vault::seed {

namespace {

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// <0, 0, >0 as `word` (its first `len` letters, lowered; the 4-letter key when
// `len` > 4) sorts before, at or after kWords[i].
int Compare(const char *word, size_t len, size_t i) {
    const char *w = kWords[i];
    for (size_t k = 0; k < len; ++k) {
        const char a = Lower(word[k]);
        if (w[k] != a) {
            return w[k] == '\0' || static_cast<unsigned char>(a) > static_cast<unsigned char>(w[k])
                       ? 1
                       : -1;
        }
    }
    return len < 4 && w[len] != '\0' ? -1 : 0;  // a short word: only whole
}

// The 11 bits at `bit` of the 132 (entropy, then the checksum's 4 bits).
uint16_t Bits(const uint8_t *bytes, size_t bit) {
    uint16_t v = 0;
    for (size_t k = 0; k < 11; ++k, ++bit) {
        v = static_cast<uint16_t>(v << 1 | ((bytes[bit / 8] >> (7 - bit % 8)) & 1));
    }
    return v;
}

}  // namespace

int Find(const char *word, size_t len) {
    if (len == 0 || len > kWordMax) {
        return -1;
    }
    const size_t key = len < 4 ? len : 4;
    size_t lo = 0;
    size_t hi = kWordListSize;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const int c = Compare(word, key, mid);
        if (c == 0) {
            // The rest of a longer word must match too ("abandox" is no word).
            for (size_t k = key; k < len; ++k) {
                if (kWords[mid][k] != Lower(word[k])) {
                    return -1;
                }
            }
            return static_cast<int>(mid);
        }
        if (c < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return -1;
}

void Encode(const uint8_t entropy[kEntropySize], uint8_t checksum, char out[kTextSize]) {
    uint8_t bytes[kEntropySize + 1];
    for (size_t i = 0; i < kEntropySize; ++i) {
        bytes[i] = entropy[i];
    }
    bytes[kEntropySize] = static_cast<uint8_t>(checksum << 4);
    size_t n = 0;
    for (size_t w = 0; w < kWordCount; ++w) {
        if (w != 0) {
            out[n++] = ' ';
        }
        for (const char *p = kWords[Bits(bytes, w * 11)]; *p != '\0'; ++p) {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    for (uint8_t &b : bytes) {
        b = 0;
    }
}

bool Parse(const char *text, uint8_t entropy[kEntropySize], uint8_t *checksum) {
    uint8_t bytes[kEntropySize + 1] = {};
    size_t words = 0;
    bool ok = true;
    for (const char *p = text; ok;) {
        while (Space(*p)) {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p != '\0' && !Space(*p)) {
            ++p;
        }
        const int index = Find(start, static_cast<size_t>(p - start));
        if (index < 0 || words == kWordCount) {
            ok = false;
            break;
        }
        for (size_t k = 0; k < 11; ++k) {
            const size_t bit = words * 11 + k;
            if ((index >> (10 - k)) & 1) {
                bytes[bit / 8] = static_cast<uint8_t>(bytes[bit / 8] | 0x80 >> (bit % 8));
            }
        }
        ++words;
    }
    ok = ok && words == kWordCount;
    for (size_t i = 0; i < kEntropySize; ++i) {
        entropy[i] = ok ? bytes[i] : 0;
    }
    *checksum = ok ? static_cast<uint8_t>(bytes[kEntropySize] >> 4) : 0;
    for (uint8_t &b : bytes) {
        b = 0;
    }
    return ok;
}

}  // namespace vault::seed
