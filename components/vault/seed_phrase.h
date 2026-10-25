#pragma once

// The seed phrase: BIP39, 12 English words = 128 random bits (the entropy)
// and a 4-bit checksum, the top bits of SHA-256(entropy). The entropy is
// mixed into the store key (vault.h): the same phrase on every device that
// shares the store files. Pure (the caller hashes), host-tested.
//
// Entry is forgiving: any case, any spaces between words, and a word may be
// cut to its first 4 letters or more ("aban" = "abandon"); the list tells all
// words apart by 4 letters. Words of 3 letters are typed whole.

#include <cstddef>
#include <cstdint>

namespace vault::seed {

inline constexpr size_t kEntropySize = 16;
inline constexpr size_t kWordCount = 12;
inline constexpr size_t kWordListSize = 2048;
inline constexpr size_t kWordMax = 8;
// 12 words of up to 8 letters, a space between, NUL.
inline constexpr size_t kTextSize = kWordCount * (kWordMax + 1);
// The phrase's id in the store headers: SHA-256(entropy), first bytes.
inline constexpr size_t kIdSize = 4;

extern const char *const kWords[kWordListSize];

// The index of a word or of its 4+ letter start (any case), -1 if none.
int Find(const char *word, size_t len);

// The checksum of a phrase from SHA-256(entropy).
inline uint8_t Checksum(const uint8_t sha256_first_byte) { return sha256_first_byte >> 4; }

// The 12 words, lower case, one space apart.
void Encode(const uint8_t entropy[kEntropySize], uint8_t checksum, char out[kTextSize]);

// False: not 12 known words. `checksum` gets the one the words carry: the
// caller compares it with Checksum(SHA-256(entropy)[0]) (a typo shows there).
bool Parse(const char *text, uint8_t entropy[kEntropySize], uint8_t *checksum);

}  // namespace vault::seed
