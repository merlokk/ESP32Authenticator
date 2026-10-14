#pragma once

// The password store: `<dir>/vault.bin`, AES-256-GCM with a key derived from
// the PIN (PBKDF2-HMAC-SHA256, salt and iterations in the store header; the
// header is the associated data, a random nonce per write). Nothing is
// decrypted until Unlock(pin); then the file is loaded whole into PSRAM and
// rewritten whole on every change. Records: records.h.
//
// Thread-safe (one mutex). A failed write reloads the file: the change is
// undone. Plaintext never leaves PSRAM except in copies the caller asked for.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "pin_guard.h"
#include "records.h"

namespace vault {

enum class State : uint8_t {
    kOff,      // no storage (Init not called, or the file system not mounted)
    kLocked,   // no PIN entered yet (or locked again)
    kOpen,     // records in memory
    kRefused,  // the file cannot be used (see Info::why); left as it is
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
    uint32_t kdf_iterations;  // of the open store
    uint32_t unlock_ms;       // the last key derivation took
    uint8_t pin_failures;     // wrong PINs in a row (kMaxPinTries locks)
    uint32_t lock_left_s;     // PIN entry locked for this long, 0: free
    bool wipe_set;            // a wipe PIN is configured
};

// From config.json `pin`; set before Unlock and again on a change.
struct Policy {
    uint32_t lock_seconds;  // after kMaxPinTries wrong PINs
    const char *wipe;       // pin_guard.h text, "" for none (copied)
};
void SetPolicy(const Policy &policy);
// Called after a wipe (outside the vault's lock): erase the FIDO2 credentials.
void SetWipeHook(void (*on_wipe)());

// PSRAM for `capacity` records; the file is in `dir` (nullptr: kOff).
esp_err_t Init(const char *dir, size_t capacity);

// Derives the key from `pin` and opens the store. No file yet: a new, empty
// store with this PIN. kWrongPin: stays locked. kLocked: refused (Info::why).
// The wipe PIN (checked first, in any state, also while locked out) erases
// the store file, the key and the records, and calls on_wipe: kWiped.
// kMaxPinTries wrong PINs in a row: kPinLocked for lock_seconds.
Error Unlock(const char *pin);
// Wipes the records and the key.
void Lock();
// A new salt and key from `new_pin`; the open store is rewritten with it.
// A wrong `old_pin` counts as a wrong PIN.
Error ChangePin(const char *old_pin, const char *new_pin);

// The pin_guard.h text of a wipe PIN (random salt). Needs the open store:
// kPinInUse if it is the PIN.
Error MakeWipe(const char *pin, char out[kWipeTextSize]);

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

// Every record gone: an empty store written over the file. Open: with its
// key (`pin` nullptr) or a new PIN; locked or refused: `pin` becomes the PIN.
Error Reset(const char *pin);

}  // namespace vault
