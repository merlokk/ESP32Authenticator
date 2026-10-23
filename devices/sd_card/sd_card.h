#pragma once

// The board's microSD card: native SDMMC, 1-bit, FAT at /sdcard (sd_policy.h
// for the mount plan). Mounted once at boot; never formatted: the card holds
// the reader's books too. No card-detect line: a card pulled out shows up as
// I/O errors.

#include <cstdint>

#include "esp_err.h"
#include "sd_policy.h"

namespace sd_card {

inline constexpr const char *kBasePath = "/sdcard";
// The store files (fido.bin, vault.bin; encrypted), apart from the reader's.
inline constexpr const char *kStoreDir = "/sdcard/.authenticator";

// After boards::Init. Not fatal: Mounted() is false and Status() says why.
esp_err_t Init();

bool Mounted();
Outcome Status();

struct Info {
    char name[8];         // from the CID
    uint64_t bytes;       // card capacity
    uint32_t khz;         // bus clock
    bool high_capacity;   // SDHC/SDXC
    uint64_t fs_total;    // FAT
    uint64_t fs_free;
};
esp_err_t GetInfo(Info *out);

// Console `sd` / hwinfo `[sd card]`.
void PrintInfo();

}  // namespace sd_card
