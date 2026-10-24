#pragma once

// The password store: `<dir>/vault.bin`, AES-256-GCM (the store header is the
// associated data, a random nonce per write).
//
// The key: PBKDF2-HMAC-SHA256(PIN, salt). Salt, count and the key's id are in
// every store header, so another device with the same PIN (and these files,
// e.g. from a sync server) derives the same key. fido.bin uses it too
// (Hooks). A PIN change makes a new key (new salt) and rewrites both stores
// from memory: each to .new, then a marker, then the renames (store_io.h);
// at boot storefile::FinishCommit(kPinChangeMarker) completes or drops it.
//
// Nothing is decrypted until Unlock(pin); then vault.bin is loaded whole into
// PSRAM and rewritten whole on every change. Records: records.h.
//
// Thread-safe (one mutex). A failed write reloads the file: the change is
// undone. Plaintext never leaves PSRAM except in copies the caller asked for.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "pin_guard.h"
#include "records.h"

namespace vault {

inline constexpr const char *kPinChangeMarker = "pinchange";  // in the store dir
inline constexpr size_t kSaltSize = 16;

struct Key {
    uint8_t key[32];
    uint8_t salt[kSaltSize];
    uint32_t iterations;
    uint8_t id[8];  // SHA-256("ESAF key id" || key), first 8 bytes
};

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
    uint32_t kdf_iterations;  // of the key in memory
    uint8_t key_id[8];        // of the key in memory
    uint32_t unlock_ms;       // the last key derivation took
    uint8_t pin_failures;     // wrong PINs in a row (kMaxPinTries locks)
    uint32_t lock_left_s;     // PIN entry locked for this long, 0: free
    bool wipe_set;            // a wipe PIN is configured
    bool has_key;             // the key is in memory
};

// From config.json `pin`; set before Unlock and again on a change.
struct Policy {
    uint32_t lock_seconds;  // after kMaxPinTries wrong PINs
    const char *wipe;       // pin_guard.h text, "" for none (copied)
    bool complexity;        // a new PIN must pass pin_policy.h (kWeakPin)
};
void SetPolicy(const Policy &policy);
// fido.bin follows the vault's key. key_in / key_gone / wipe run outside the
// vault's lock; stage / commit / unstage inside a PIN change (fido.bin.new
// with the new key, then over fido.bin). wipe: quietly (it looks like a
// wrong PIN). stage returns ESP_ERR_INVALID_STATE for a file it cannot
// re-key (not loaded): the change is refused.
struct Hooks {
    void (*key_in)(const Key &key);
    void (*key_gone)();
    esp_err_t (*stage)(const Key &key);
    void (*commit)();
    void (*unstage)();
    void (*wipe)();
};
void SetHooks(const Hooks &hooks);

// PSRAM for `capacity` records; the file is in `dir` (nullptr: kOff).
esp_err_t Init(const char *dir, size_t capacity);

// Derives the key from `pin` and the salt in vault.bin and opens the store
// (a wrong key id: kWrongPin, stays locked). No vault.bin yet: a new store and
// key (new salt). kLocked: refused (Info::why; the key may be in, see
// Info::has_key).
// The wipe PIN (checked first, in any state, also while locked out) erases
// the store file, the key and the records, and calls on_wipe; it answers as a
// wrong PIN (kWrongPin / kPinLocked, counted, same time, nothing logged).
// kMaxPinTries wrong PINs in a row: kPinLocked for lock_seconds.
Error Unlock(const char *pin);
// Wipes the records and the key.
void Lock();
// A copy of the key in memory (kept through a sleep, power.h); false: none.
// Wipe it after use.
bool CopyKey(Key *out);
// Opens the store with a key kept through a sleep, no PIN (nothing counted):
// kOk, or kLocked (another key, no or a damaged file: the key dropped, the
// PIN as ever), kPinLocked while PIN entry is locked.
Error Open(const Key &key);
// A new key from `new_pin` (new salt); vault.bin and fido.bin are written
// with it from memory. Needs the store open. A wrong `old_pin` counts as a
// wrong PIN. kFidoNotLoaded: fido.bin exists but is not loaded (it could not
// be re-keyed): nothing changed.
Error ChangePin(const char *old_pin, const char *new_pin);

// The pin_guard.h text of a wipe PIN (random salt). Needs the open store:
// kPinInUse if it is the PIN.
Error MakeWipe(const char *pin, char out[kWipeTextSize]);

Info GetInfo();
// The state alone, without the lock (Unlock holds it for the key derivation,
// a second or more): for a screen that polls.
State GetState();

// A copy of the record at list position `index` (the user's order, records.h).
// It holds the password: wipe it (vault::Wipe) after use.
Error Get(size_t index, Record *out);
// List position of the record with this ID, -1 if none (or not open).
long Position(uint32_t id);

Error Add(const char *site, const char *user, const char *password, const char *otp,
          uint32_t *id);
Error Set(uint32_t id, Field field, const char *value);
Error Remove(uint32_t id);
// The list order (the screen's and the site's): one record to `position`,
// or all by site, then user.
Error Move(uint32_t id, size_t position);
Error SortByName();

// Every record gone: an empty store written over the file. With the key in,
// `pin` is not needed; without, `pin` makes a new key (new salt): fido.bin,
// if any, then has another key (`fido2 reset`).
Error Reset(const char *pin);

}  // namespace vault
