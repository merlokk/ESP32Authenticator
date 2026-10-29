#pragma once

// The password store: `<dir>/vault.bin`, AES-256-GCM (the store header is the
// associated data, a random nonce per write).
//
// The key: HKDF-SHA256(salt = seed, IKM = PBKDF2-HMAC-SHA256(PIN, salt),
// info "ESAF store key"); the seed is the seed phrase's 16 bytes
// (seed_phrase.h), kept in its own file on SPIFFS (never next to the stores),
// the same on every device that shares the store files. Salt, count, the
// key's id and the seed's id are in every store header, so another device
// with the PIN and the phrase (and these files, e.g. from a sync server)
// derives the same key; the files alone do not open whatever the PIN.
// fido.bin uses the key too (Hooks). A PIN or seed change makes a new key
// (new salt) and rewrites both stores from memory: each to .new (the seed
// file too), then a marker, then the renames (store_io.h); at boot
// storefile::FinishCommit(kPinChangeMarker) completes or drops it.
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
#include "seed_phrase.h"
#include "store_io.h"

namespace vault {

inline constexpr const char *kPinChangeMarker = "pinchange";  // in the store dir
inline constexpr size_t kSaltSize = 16;

struct Key {
    uint8_t key[32];
    uint8_t salt[kSaltSize];
    uint32_t iterations;
    uint8_t id[8];  // SHA-256("ESAF key id" || key), first 8 bytes
    uint8_t seed_id[seed::kIdSize];  // of the seed phrase in it
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
    bool seed_set;            // a seed phrase on this device
    uint8_t seed_id[seed::kIdSize];       // of it
    uint8_t file_seed_id[seed::kIdSize];  // the one vault.bin names, zero if unread
    uint8_t store_id[8];                  // vault.bin's, zero if unread
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
// re-key (not loaded): the change is refused. adopt (inside the lock): the
// sync's copy of fido.bin, checked, from `from` over the file and loaded
// with `key` (`force`: its version kept as it is, another store taken).
struct Hooks {
    void (*key_in)(const Key &key);
    void (*key_gone)();
    esp_err_t (*stage)(const Key &key);
    void (*commit)();
    void (*unstage)();
    void (*wipe)();
    esp_err_t (*adopt)(const char *from, const Key &key, bool force);
};
void SetHooks(const Hooks &hooks);

// PSRAM for `capacity` records; the file is in `dir` (nullptr: kOff); the
// seed phrase in `seed_path` (read now; none: the first new store makes one).
esp_err_t Init(const char *dir, size_t capacity, const char *seed_path);

// Derives the key from `pin` and the salt in vault.bin and opens the store
// (a wrong key id: kWrongPin, stays locked). No vault.bin yet: a new store and
// key (new salt). kLocked: refused (Info::why; the key may be in, see
// Info::has_key).
// No seed phrase, or another one than the file names: kNoSeed / kWrongSeed,
// nothing counted.
// The wipe PIN (checked first, in any state, also while locked out) erases
// the store file, the key, the records and the seed phrase, and calls
// on_wipe; it answers as a
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

// The seed phrase. Without the key in: `words` become this device's phrase
// (e.g. the one of another device whose files this one gets); nullptr, a
// random one, only while there is no store yet (else kLocked). With the store
// open: `pin` (counted as ever) and a new key with `words` (nullptr: random)
// for vault.bin and fido.bin, as ChangePin. kBadSeed: not 12 words or a typo.
// , if given, gets the new phrase's words (wipe it after use).
Error SetSeed(const char *pin, const char *words, char *out = nullptr);
// The words of the seed phrase (to write down, or for another device); needs
// the store open and the PIN (counted). Wipe `out` after use.
Error CopySeed(const char *pin, char out[seed::kTextSize]);

// The pin_guard.h text of a wipe PIN (random salt). Needs the open store:
// kPinInUse if it is the PIN.
Error MakeWipe(const char *pin, char out[kWipeTextSize]);

Info GetInfo();
// The seed phrase's id into `out`; false: none. Memory only (no flash or SD:
// for a task whose stack is in PSRAM).
bool SeedId(uint8_t out[seed::kIdSize]);
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

// --- The sync (server/sync_logic.h)

// vault.bin's File (path, kind, NVS), for the sync to read it.
const storefile::File &StoreFile();
// The loaded vault.bin's version, 0 while not open. Quick (no lock).
uint32_t Version();

// A copy of a store from the server, downloaded to `from`, put in place of
// `target` (StoreFile() or fido.bin's): checked first, the kind, our seed
// phrase (kWrongSeed), `store_id` (the one asked for), a version above the
// kept one (unless `other_store`: an account's store taken instead of ours,
// its version kept as it is), the key: ours, or the one `pin` gives with
// the copy's salt (no `pin`: kNeedPin; a wrong one counted as ever:
// kWrongPin / kPinLocked), the GCM tag (kBadCopy). Another key: this device
// takes it, the other store re-keyed with it as a PIN change does, so this
// device's PIN becomes the copy's. vault.bin reloaded; fido.bin through
// Hooks::adopt. Needs the store open (kLocked). `from` is removed, but kept
// on kNeedPin / kWrongPin (for the PIN typed next).
Error Adopt(const storefile::File &target, const char *from,
            const uint8_t store_id[storefile::kStoreIdSize], bool other_store, const char *pin);

// vault.bin written again with a version of at least `min_version` (this
// device's copy kept over the server's). kLocked: not open.
Error Rewrite(uint32_t min_version);

// Every record gone: an empty store written over the file. With the key in,
// `pin` is not needed; without, `pin` makes a new key (new salt): fido.bin,
// if any, then has another key (`fido2 reset`).
Error Reset(const char *pin);

}  // namespace vault
