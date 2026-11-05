#pragma once

// The generator's word lists, in the firmware (flash): EFF large and SLIP-39
// here, BIP-39 the seed phrase's (vault/seed_words.cpp).

#include <cstddef>

#include "passgen.h"

namespace passgen {

inline constexpr size_t kEffSize = 7772;
inline constexpr size_t kSlip39Size = 1024;

extern const char *const kEff[kEffSize];
extern const char *const kSlip39[kSlip39Size];

// A list's words.
Words WordsOf(List list);

}  // namespace passgen
