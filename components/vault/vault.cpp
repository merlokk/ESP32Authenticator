#include "vault.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#include "clock_math.h"
#include "crypto.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "store_io.h"

namespace vault {

namespace {

constexpr const char *TAG = "vault";
constexpr const char *kFileName = "vault.bin";
constexpr const char *kNvsNamespace = "vault";
// PBKDF2 rounds for a new key (a file keeps the count it was made with).
constexpr uint32_t kKdfIterations = 6000;  // ~0.9 s on the S3 (PSA, ~150 us each)
constexpr const char *kNvsFailures = "pin_fail";   // u8: wrong PINs in a row
constexpr const char *kNvsDeadline = "pin_until";  // i64: lock end, UTC (0 unknown)
static_assert(kSaltSize == storefile::kSaltSize);
static_assert(sizeof(Key::id) == storefile::kKeyIdSize);

StaticSemaphore_t g_lock_storage;
SemaphoreHandle_t g_lock = nullptr;

State g_state = State::kOff;
const char *g_why = nullptr;
char g_path[64];
char g_marker[64];
storefile::File g_file = {g_path, storefile::Kind::kRecords, kNvsNamespace};
storefile::Header g_header;  // vault.bin as loaded / last written

// The store key: PBKDF2(PIN, salt). Also fido.bin's (Hooks).
Key g_key;
bool g_have_key = false;
uint32_t g_unlock_ms = 0;
Hooks g_hooks = {};

// PIN policy (config.json `pin`) and the lockout (pin_guard.h).
uint32_t g_lock_seconds = 600;
bool g_has_wipe = false;
uint8_t g_wipe_salt[kWipeSaltSize];
uint8_t g_wipe_hash[kWipeHashSize];
uint8_t g_failures = 0;
int64_t g_lock_until_us = 0;  // uptime clock; 0: not locked

// PSRAM: the records, their serialized plaintext and the file body.
Record *g_slots = nullptr;
size_t g_capacity = 0;
uint8_t *g_plain = nullptr;
uint8_t *g_body = nullptr;
size_t g_plain_capacity = 0;
alignas(Records) uint8_t g_records_storage[sizeof(Records)];
Records *g_records = nullptr;

class Guard {
public:
    Guard() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(g_lock); }
};

void *PsramCalloc(size_t size) {
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != nullptr ? p : heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
}

int64_t Now() {
    const time_t now = time(nullptr);
    return rtc::Believable(now) ? static_cast<int64_t>(now) : 0;
}

// --- lockout

// NVS: only on a change (a few writes per wrong PIN at most).
void StoreFailures(uint8_t failures, int64_t deadline_utc) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, kNvsFailures, failures);
    nvs_set_i64(h, kNvsDeadline, deadline_utc);
    nvs_commit(h);
    nvs_close(h);
}

void LoadFailures() {
    nvs_handle_t h;
    uint8_t failures = 0;
    int64_t deadline = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, kNvsFailures, &failures);
        nvs_get_i64(h, kNvsDeadline, &deadline);
        nvs_close(h);
    }
    g_failures = failures;
    const uint32_t left = LockSecondsAtBoot(failures, deadline, Now(), g_lock_seconds);
    if (left > 0) {
        g_lock_until_us = esp_timer_get_time() + static_cast<int64_t>(left) * 1000000;
        ESP_LOGW(TAG, "PIN entry locked for %u s (%u wrong PINs)", static_cast<unsigned>(left),
                 static_cast<unsigned>(failures));
    }
}

uint32_t LockLeft() {
    const int64_t left_us = g_lock_until_us - esp_timer_get_time();
    return left_us > 0 ? static_cast<uint32_t>((left_us + 999999) / 1000000) : 0;
}

// Before a PIN is checked. A lock that ran out gives kMaxPinTries more tries.
bool PinEntryAllowed() {
    if (LockLeft() > 0) {
        return false;
    }
    if (g_failures >= kMaxPinTries) {
        g_failures = 0;
        g_lock_until_us = 0;
        StoreFailures(0, 0);
    }
    return true;
}

void PinRight() {
    if (g_failures != 0) {
        g_failures = 0;
        StoreFailures(0, 0);
    }
}

Error PinWrong() {
    ++g_failures;
    if (g_failures < kMaxPinTries) {
        StoreFailures(g_failures, 0);
        return Error::kWrongPin;
    }
    g_lock_until_us = esp_timer_get_time() + static_cast<int64_t>(g_lock_seconds) * 1000000;
    const int64_t now = Now();
    StoreFailures(g_failures, now != 0 ? now + g_lock_seconds : 0);
    ESP_LOGW(TAG, "%u wrong PINs: PIN entry locked for %u s", static_cast<unsigned>(g_failures),
             static_cast<unsigned>(g_lock_seconds));
    return Error::kPinLocked;
}

// --- wipe PIN

void WipeHash(const char *pin, const uint8_t salt[kWipeSaltSize], uint8_t out[kWipeHashSize]) {
    uint8_t text[kWipeSaltSize + kPinLength];
    memcpy(text, salt, kWipeSaltSize);
    memcpy(text + kWipeSaltSize, pin, kPinLength);
    crypto::Sha256(text, sizeof text, out);
    Wipe(text, sizeof text);
}

bool IsWipePin(const char *pin) {
    if (!g_has_wipe) {
        return false;
    }
    uint8_t hash[kWipeHashSize];
    WipeHash(pin, g_wipe_salt, hash);
    const bool same = memcmp(hash, g_wipe_hash, kWipeHashSize) == 0;
    Wipe(hash, sizeof hash);
    return same;
}

// --- the key

// `out` = PBKDF2(pin, salt, iterations) and its id.
esp_err_t Derive(const char *pin, const uint8_t salt[kSaltSize], uint32_t iterations, Key *out) {
    const int64_t start = esp_timer_get_time();
    esp_err_t err =
        crypto::Pbkdf2Sha256(reinterpret_cast<const uint8_t *>(pin), strlen(pin), salt,
                             kSaltSize, iterations, out->key, sizeof out->key);
    g_unlock_ms = static_cast<uint32_t>((esp_timer_get_time() - start) / 1000);
    ESP_LOGI(TAG, "key derived: %u iterations, %u ms", static_cast<unsigned>(iterations),
             static_cast<unsigned>(g_unlock_ms));
    memcpy(out->salt, salt, kSaltSize);
    out->iterations = iterations;
    // The id: SHA-256("ESAF key id" || key), first 8 bytes.
    uint8_t text[11 + sizeof out->key];
    memcpy(text, "ESAF key id", 11);
    memcpy(text + 11, out->key, sizeof out->key);
    uint8_t hash[crypto::kSha256Size];
    if (err == ESP_OK) {
        err = crypto::Sha256(text, sizeof text, hash);
    }
    memcpy(out->id, hash, sizeof out->id);
    Wipe(text, sizeof text);
    return err;
}

// A new random salt.
esp_err_t DeriveNew(const char *pin, Key *out) {
    uint8_t salt[kSaltSize];
    crypto::Random(salt, sizeof salt);
    return Derive(pin, salt, kKdfIterations, out);
}

bool SameKey(const Key &a, const Key &b) {
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof a.key; ++i) {
        diff |= a.key[i] ^ b.key[i];
    }
    return diff == 0;
}

// Does `pin` give the key in memory? (One derivation.)
bool PinMatches(const char *pin) {
    Key k;
    const bool same = Derive(pin, g_key.salt, g_key.iterations, &k) == ESP_OK && SameKey(k, g_key);
    Wipe(&k, sizeof k);
    return same;
}

void ForgetKey() {
    Wipe(&g_key, sizeof g_key);
    g_have_key = false;
}

// --- vault.bin

void Refuse(const char *why) {
    g_records->Clear();
    g_state = State::kRefused;
    g_why = why;
    ESP_LOGE(TAG, "%s not loaded: %s", g_path, why);
}

void Opened() {
    g_state = State::kOpen;
    g_why = nullptr;
    ESP_LOGI(TAG, "%s: version %u, %u record(s)", g_path, static_cast<unsigned>(g_header.version),
             static_cast<unsigned>(g_records->Count()));
}

// The records sealed with `key` into a header and g_body. Returns the body
// size, 0 on a failure.
size_t Seal(const Key &key, storefile::Header *h) {
    const size_t n = g_records->Serialize(g_plain, g_plain_capacity);
    if (n == 0) {
        return 0;  // never: the buffer holds a full store
    }
    *h = storefile::Header();
    h->kind = storefile::Kind::kRecords;
    h->cipher = storefile::Cipher::kAesGcm;
    h->version = storefile::NextVersion(g_file, g_header.version);
    h->length = static_cast<uint32_t>(n);
    h->modified_utc = Now();
    crypto::Random(h->nonce, sizeof h->nonce);
    h->kdf_iterations = key.iterations;
    memcpy(h->salt, key.salt, sizeof h->salt);
    memcpy(h->key_id, key.id, sizeof h->key_id);
    uint8_t aad[storefile::kHeaderSize];
    storefile::Encode(*h, aad);
    const esp_err_t err = crypto::Aes256GcmSeal(key.key, h->nonce, aad, sizeof aad, g_plain, n,
                                                g_body);
    Wipe(g_plain, n);
    return err == ESP_OK ? n + storefile::kTagSize : 0;
}

// Records -> vault.bin, with the key.
esp_err_t Save() {
    storefile::Header h;
    const size_t size = Seal(g_key, &h);
    esp_err_t err = size == 0 ? ESP_FAIL : storefile::Save(g_file, h, g_body, size);
    if (err == ESP_OK) {
        g_header = h;
    }
    return err;
}

enum class Read { kOk, kNew, kWrongPin, kRefused };

// vault.bin into the records. With `pin`: the key from it and the file's
// salt (g_key on success); without: g_key. kRefused: Refuse() done.
Read ReadFile(const char *pin) {
    g_records->Clear();
    uint8_t raw[storefile::kHeaderSize];
    storefile::Header h;
    size_t body_size = 0;
    const char *why = nullptr;
    const storefile::Loaded loaded = storefile::Load(
        g_file, g_body, g_plain_capacity + storefile::kTagSize, &h, &body_size, raw, &why);
    if (loaded == storefile::Loaded::kRefused) {
        Refuse(why);
        return Read::kRefused;
    }
    if (loaded == storefile::Loaded::kNew) {
        g_header = storefile::Header();
        return Read::kNew;
    }
    if (h.cipher != storefile::Cipher::kAesGcm || h.kdf_iterations == 0) {
        Refuse("the store file is not encrypted with a PIN key");
        return Read::kRefused;
    }
    if (pin != nullptr) {
        Key k;
        const esp_err_t err = Derive(pin, h.salt, h.kdf_iterations, &k);
        if (err != ESP_OK || memcmp(k.id, h.key_id, sizeof k.id) != 0) {
            Wipe(&k, sizeof k);
            return err == ESP_OK ? Read::kWrongPin : Read::kRefused;
        }
        g_key = k;
        g_have_key = true;
        Wipe(&k, sizeof k);
    } else if (memcmp(g_key.id, h.key_id, sizeof h.key_id) != 0) {
        Refuse("the store file has another key");
        return Read::kRefused;
    }
    const esp_err_t err =
        crypto::Aes256GcmOpen(g_key.key, h.nonce, raw, sizeof raw, g_body, h.length, g_plain);
    if (err != ESP_OK) {
        Refuse(err == ESP_ERR_INVALID_CRC ? "the store file is damaged (it does not decrypt)"
                                          : "decryption failed");
        return Read::kRefused;
    }
    const Error e = g_records->Parse(g_plain, h.length);
    Wipe(g_plain, h.length);
    if (e != Error::kOk) {
        Refuse(e == Error::kFull ? "the store file holds more records than fit"
                                 : "the store file's records are malformed");
        return Read::kRefused;
    }
    g_header = h;
    return Read::kOk;
}

// After a change in memory: write it, or reload the file (the change undone).
Error Changed(Error e) {
    if (e != Error::kOk) {
        return e;
    }
    const esp_err_t err = Save();
    if (err == ESP_OK) {
        return Error::kOk;
    }
    ESP_LOGE(TAG, "save failed (%s): reloading", esp_err_to_name(err));
    if (ReadFile(nullptr) == Read::kOk) {
        Opened();
    }
    return Error::kStorage;
}

// The wipe PIN: vault.bin, its version, the key and the records gone; the
// next PIN starts afresh. Nothing logged: the log is on the console.
void WipeStore() {
    g_records->Clear();
    ForgetKey();
    storefile::Unstage(g_file);
    storefile::Forget(g_file);
    storefile::EndCommit(g_marker);
    g_header = storefile::Header();
    g_state = State::kLocked;
    g_why = nullptr;
}

// What happens to the key, for the hooks (run outside the lock).
enum class KeyEvent { kNone, kIn, kGone };

void Notify(KeyEvent event) {
    if (event == KeyEvent::kGone) {
        if (g_hooks.key_gone != nullptr) {
            g_hooks.key_gone();
        }
        return;
    }
    if (event != KeyEvent::kIn || g_hooks.key_in == nullptr) {
        return;
    }
    Key key;
    {
        Guard guard;
        if (!g_have_key) {
            return;
        }
        key = g_key;
    }
    g_hooks.key_in(key);
    Wipe(&key, sizeof key);
}

// Under the lock.
Error UnlockLocked(const char *pin, bool *wiped, KeyEvent *event) {
    if (IsWipePin(pin)) {
        // Erase, then answer exactly as a wrong PIN on a locked store would:
        // the same result, the same count, the same time (one derivation),
        // the same log line.
        WipeStore();
        *wiped = true;
        if (!PinEntryAllowed()) {
            return Error::kPinLocked;
        }
        Key k;
        DeriveNew(pin, &k);
        Wipe(&k, sizeof k);
        ESP_LOGW(TAG, "unlock: wrong PIN");
        return PinWrong();
    }
    if (!PinEntryAllowed()) {
        return Error::kPinLocked;
    }
    if (g_have_key) {
        // Already in: only checked, nothing changes either way.
        if (!PinMatches(pin)) {
            return PinWrong();
        }
        PinRight();
        return Error::kOk;
    }
    switch (ReadFile(pin)) {
        case Read::kOk:
            break;
        case Read::kNew:
            // No store yet: this PIN makes the key (a new salt).
            if (DeriveNew(pin, &g_key) != ESP_OK) {
                ForgetKey();
                return Error::kStorage;
            }
            g_have_key = true;
            if (Save() != ESP_OK) {
                ForgetKey();
                return Error::kStorage;
            }
            ESP_LOGW(TAG, "new store, its key from this PIN");
            break;
        case Read::kWrongPin:
            ESP_LOGW(TAG, "unlock: wrong PIN");
            return PinWrong();
        case Read::kRefused:
            // The PIN was right when the key is in (a damaged file).
            if (g_have_key) {
                PinRight();
                *event = KeyEvent::kIn;
            }
            return Error::kLocked;
    }
    PinRight();
    Opened();
    *event = KeyEvent::kIn;
    return Error::kOk;
}

}  // namespace

const char *StateName(State state) {
    switch (state) {
        case State::kOff:
            return "off";
        case State::kLocked:
            return "locked";
        case State::kOpen:
            return "open";
        case State::kRefused:
            return "refused";
    }
    return "?";
}

esp_err_t Init(const char *dir, size_t capacity) {
    if (g_lock != nullptr || capacity == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "no storage: the password store is off");
        return ESP_OK;
    }
    snprintf(g_path, sizeof g_path, "%s/%s", dir, kFileName);
    snprintf(g_marker, sizeof g_marker, "%s/%s", dir, kPinChangeMarker);
    g_capacity = capacity;
    g_plain_capacity = MaxSerialized(capacity);
    g_slots = static_cast<Record *>(PsramCalloc(capacity * sizeof(Record)));
    g_plain = static_cast<uint8_t *>(PsramCalloc(g_plain_capacity));
    g_body = static_cast<uint8_t *>(PsramCalloc(g_plain_capacity + storefile::kTagSize));
    if (g_slots == nullptr || g_plain == nullptr || g_body == nullptr) {
        ESP_LOGE(TAG, "no memory for %u records", static_cast<unsigned>(capacity));
        return ESP_ERR_NO_MEM;
    }
    g_records = new (g_records_storage) Records(g_slots, capacity);
    g_state = State::kLocked;
    LoadFailures();
    ESP_LOGI(TAG, "%s, up to %u records, %u bytes of PSRAM; locked until the PIN", g_path,
             static_cast<unsigned>(capacity),
             static_cast<unsigned>(capacity * sizeof(Record) + 2 * g_plain_capacity));
    return ESP_OK;
}

void SetPolicy(const Policy &policy) {
    uint8_t salt[kWipeSaltSize];
    uint8_t hash[kWipeHashSize];
    const bool has_wipe = policy.wipe != nullptr && ParseWipe(policy.wipe, salt, hash);
    if (g_lock != nullptr) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
    }
    g_lock_seconds = policy.lock_seconds;
    g_has_wipe = has_wipe;
    memcpy(g_wipe_salt, salt, sizeof salt);
    memcpy(g_wipe_hash, hash, sizeof hash);
    if (g_lock != nullptr) {
        xSemaphoreGive(g_lock);
    }
}

void SetHooks(const Hooks &hooks) { g_hooks = hooks; }

Error Unlock(const char *pin) {
    if (!ValidPin(pin)) {
        return Error::kBadPin;
    }
    if (g_state == State::kOff) {
        return Error::kLocked;
    }
    Error e;
    bool wiped = false;
    KeyEvent event = KeyEvent::kNone;
    {
        Guard guard;
        e = UnlockLocked(pin, &wiped, &event);
    }
    if (wiped && g_hooks.wipe != nullptr) {
        g_hooks.wipe();
    }
    Notify(event);
    return e;
}

void Lock() {
    if (g_state == State::kOff) {
        return;
    }
    {
        Guard guard;
        g_records->Clear();
        ForgetKey();
        g_state = State::kLocked;
        g_why = nullptr;
    }
    Notify(KeyEvent::kGone);
}

Error ChangePin(const char *old_pin, const char *new_pin) {
    if (!ValidPin(old_pin) || !ValidPin(new_pin)) {
        return Error::kBadPin;
    }
    if (g_state != State::kOpen) {
        return Error::kLocked;
    }
    Guard guard;
    if (!PinEntryAllowed()) {
        return Error::kPinLocked;
    }
    if (!PinMatches(old_pin)) {
        return PinWrong();
    }
    PinRight();
    if (IsWipePin(new_pin)) {
        return Error::kPinInUse;
    }
    // A new key (new salt). Both stores are in memory: each is written to
    // .new with it, then the marker, then the renames (store_io.h).
    Key fresh;
    storefile::Header h;
    size_t size = 0;
    esp_err_t err = DeriveNew(new_pin, &fresh);
    if (err == ESP_OK) {
        size = Seal(fresh, &h);
        err = size == 0 ? ESP_FAIL : storefile::Stage(g_file, h, g_body, size);
    }
    if (err == ESP_OK && g_hooks.stage != nullptr) {
        err = g_hooks.stage(fresh);
    }
    if (err == ESP_OK) {
        err = storefile::BeginCommit(g_marker);
    }
    if (err != ESP_OK) {
        storefile::Unstage(g_file);
        if (g_hooks.unstage != nullptr) {
            g_hooks.unstage();
        }
        Wipe(&fresh, sizeof fresh);
        ESP_LOGE(TAG, "PIN change failed (%s): nothing changed", esp_err_to_name(err));
        return err == ESP_ERR_INVALID_STATE ? Error::kFidoNotLoaded : Error::kStorage;
    }
    // Committed: from here a power loss is finished at the next boot.
    storefile::Promote(g_file, h.version);
    if (g_hooks.commit != nullptr) {
        g_hooks.commit();
    }
    storefile::EndCommit(g_marker);
    g_key = fresh;
    g_header = h;
    Wipe(&fresh, sizeof fresh);
    ESP_LOGW(TAG, "PIN changed: a new key for every store");
    return Error::kOk;
}

Error MakeWipe(const char *pin, char out[kWipeTextSize]) {
    if (!ValidPin(pin)) {
        return Error::kBadPin;
    }
    if (!g_have_key) {
        return Error::kLocked;
    }
    Guard guard;
    if (PinMatches(pin)) {
        return Error::kPinInUse;
    }
    uint8_t salt[kWipeSaltSize];
    uint8_t hash[kWipeHashSize];
    crypto::Random(salt, sizeof salt);
    WipeHash(pin, salt, hash);
    FormatWipe(salt, hash, out);
    return Error::kOk;
}

Info GetInfo() {
    Info info = {};
    info.state = g_state;
    if (g_state == State::kOff) {
        return info;
    }
    Guard guard;
    info.why = g_why;
    info.path = g_path;
    info.count = g_state == State::kOpen ? g_records->Count() : 0;
    info.capacity = g_capacity;
    info.version = g_header.version;
    info.kept_version = storefile::KeptVersion(g_file);
    info.modified_utc = g_header.modified_utc;
    info.file_size = storefile::FileSize(g_file);
    info.has_key = g_have_key;
    info.kdf_iterations = g_have_key ? g_key.iterations : 0;
    if (g_have_key) {
        memcpy(info.key_id, g_key.id, sizeof info.key_id);
    }
    info.unlock_ms = g_unlock_ms;
    info.pin_failures = g_failures;
    info.lock_left_s = LockLeft();
    info.wipe_set = g_has_wipe;
    return info;
}

Error Get(size_t index, Record *out) {
    if (g_state != State::kOpen) {
        return Error::kLocked;
    }
    Guard guard;
    if (index >= g_records->Count()) {
        return Error::kNotFound;
    }
    *out = g_records->At(index);
    return Error::kOk;
}

long Position(uint32_t id) {
    if (g_state != State::kOpen) {
        return -1;
    }
    Guard guard;
    return g_records->Find(id);
}

Error Add(const char *site, const char *user, const char *password, const char *otp,
          uint32_t *id) {
    if (g_state != State::kOpen) {
        return Error::kLocked;
    }
    Guard guard;
    return Changed(g_records->Add(site, user, password, otp, Now(), id));
}

Error Set(uint32_t id, Field field, const char *value) {
    if (g_state != State::kOpen) {
        return Error::kLocked;
    }
    Guard guard;
    return Changed(g_records->Set(id, field, value, Now()));
}

Error Remove(uint32_t id) {
    if (g_state != State::kOpen) {
        return Error::kLocked;
    }
    Guard guard;
    return Changed(g_records->Remove(id));
}

Error Reset(const char *pin) {
    if (g_state == State::kOff) {
        return Error::kLocked;
    }
    if (pin != nullptr && !ValidPin(pin)) {
        return Error::kBadPin;
    }
    KeyEvent event = KeyEvent::kNone;
    Error e = Error::kOk;
    {
        Guard guard;
        if (!g_have_key) {
            // No key in: `pin` makes a new one (new salt). fido.bin, if any,
            // then has another key: `fido2 reset`.
            if (pin == nullptr) {
                return Error::kLocked;
            }
            if (IsWipePin(pin)) {
                return Error::kPinInUse;
            }
            if (DeriveNew(pin, &g_key) != ESP_OK) {
                ForgetKey();
                return Error::kStorage;
            }
            g_have_key = true;
            event = KeyEvent::kIn;
        }
        g_records->Clear();
        if (Save() != ESP_OK) {
            Refuse("the store file could not be written");
            e = Error::kStorage;
        } else {
            ESP_LOGW(TAG, "%s reset: every record deleted", g_path);
            Opened();
        }
    }
    Notify(event);
    return e;
}

}  // namespace vault
