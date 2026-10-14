#pragma once

// The password store: `<dir>/vault.bin`, AES-256-GCM (store header as
// associated data, random nonce per write), loaded whole and decrypted into
// PSRAM at unlock, rewritten whole on every change. Records: records.h.
//
// Thread-safe (one mutex). A failed write reloads the file: the change is
// undone. Plaintext never leaves PSRAM except in copies the caller asked for.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "records.h"

namespace vault {

inline constexpr size_t kKeySize = 32;

enum class State : uint8_t {
    kOff,      // no storage (Init not called, or the file system not mounted)
    kLocked,   // no key yet
    kOpen,     // records in memory
    kRefused,  // the file was not loaded (see Info::why); left as it is
};

const char *StateName(State state);

struct Info {
    State state;
    const char *why;  // kRefused: the reason
    const char *path;
    size_t count;
    size_t capacity;
    uint32_t version;       // of the loaded / last written file, 0 if none
    uint32_t kept_version;  // highest version this device accepted (NVS)
    int64_t modified_utc;   // last change, 0 unknown
    size_t file_size;
};

// PSRAM for `capacity` records; the file is in `dir` (nullptr: kOff).
esp_err_t Init(const char *dir, size_t capacity);

// Loads and decrypts the file with `key` (copied; wiped by Lock()). kOk, or
// kLocked with the store refused (wrong key, damaged, rollback, missing).
Error Unlock(const uint8_t key[kKeySize]);
// Wipes the records and the key.
void Lock();

Info GetInfo();

// A copy of the record at list position `index` (sorted by site, user). It
// holds the password: wipe it (vault::Wipe) after use.
Error Get(size_t index, Record *out);
// List position of the record with this ID, -1 if none (or not open).
long Position(uint32_t id);

Error Add(const char *site, const char *user, const char *password, const char *otp,
          uint32_t *id);
Error Set(uint32_t id, Field field, const char *value);
Error Remove(uint32_t id);

// Every record gone; also the way out of a refused file (written over it).
// Needs the key (unlocked or refused).
Error Reset();

// Interim key until the PIN derives one (storage-encryption.md): 32 random
// bytes in NVS (`vault/key`), made on first use. Protects nothing against a
// flash dump; it keeps the file format and code path final.
esp_err_t InterimKey(uint8_t out[kKeySize]);

}  // namespace vault
