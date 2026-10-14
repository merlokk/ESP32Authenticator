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
constexpr size_t kKeySize = crypto::kAes256KeySize;
constexpr const char *kNvsFailures = "pin_fail";   // u8: wrong PINs in a row
constexpr const char *kNvsDeadline = "pin_until";  // i64: lock end, UTC (0 unknown)

StaticSemaphore_t g_lock_storage;
SemaphoreHandle_t g_lock = nullptr;

State g_state = State::kOff;
const char *g_why = nullptr;
char g_path[64];
storefile::File g_file = {g_path, storefile::Kind::kRecords, kNvsNamespace};
storefile::Header g_header;  // of the loaded / last written file

// The open store's key and how it was derived (kept for writes).
uint8_t g_key[kKeySize];
uint8_t g_salt[storefile::kSaltSize];
uint32_t g_iterations = 0;
uint32_t g_unlock_ms = 0;

// PIN policy (config.json `pin`) and the lockout (pin_guard.h).
uint32_t g_lock_seconds = 600;
bool g_has_wipe = false;
uint8_t g_wipe_salt[kWipeSaltSize];
uint8_t g_wipe_hash[kWipeHashSize];
void (*g_on_wipe)() = nullptr;
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
    uint8_t diff = 0;
    for (size_t i = 0; i < kWipeHashSize; ++i) {
        diff |= hash[i] ^ g_wipe_hash[i];
    }
    Wipe(hash, sizeof hash);
    return diff == 0;
}

void ForgetKey() {
    Wipe(g_key, sizeof g_key);
    g_iterations = 0;
}

// g_key from `pin` with this salt and count.
esp_err_t Derive(const char *pin, const uint8_t salt[storefile::kSaltSize], uint32_t iterations) {
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = crypto::Pbkdf2Sha256(reinterpret_cast<const uint8_t *>(pin),
                                               strlen(pin), salt, storefile::kSaltSize,
                                               iterations, g_key, sizeof g_key);
    g_unlock_ms = static_cast<uint32_t>((esp_timer_get_time() - start) / 1000);
    if (err == ESP_OK) {
        memcpy(g_salt, salt, sizeof g_salt);
        g_iterations = iterations;
        ESP_LOGI(TAG, "key derived: %u iterations, %u ms", static_cast<unsigned>(iterations),
                 static_cast<unsigned>(g_unlock_ms));
    }
    return err;
}

// A new random salt and the key from `pin`.
esp_err_t DeriveNew(const char *pin) {
    uint8_t salt[storefile::kSaltSize];
    crypto::Random(salt, sizeof salt);
    return Derive(pin, salt, kKdfIterations);
}

// Does `pin` give the open key? g_key, g_salt, g_iterations unchanged.
bool PinMatches(const char *pin) {
    uint8_t open_key[kKeySize];
    memcpy(open_key, g_key, sizeof open_key);
    uint8_t salt[storefile::kSaltSize];
    memcpy(salt, g_salt, sizeof salt);
    bool same = false;
    if (Derive(pin, salt, g_iterations) == ESP_OK) {
        uint8_t diff = 0;
        for (size_t i = 0; i < kKeySize; ++i) {
            diff |= g_key[i] ^ open_key[i];
        }
        same = diff == 0;
    }
    memcpy(g_key, open_key, sizeof g_key);
    Wipe(open_key, sizeof open_key);
    return same;
}

void Refuse(const char *why) {
    g_records->Clear();
    ForgetKey();
    g_state = State::kRefused;
    g_why = why;
    ESP_LOGE(TAG, "%s not loaded: %s", g_path, why);
}

// The wipe PIN: the store file, its version, the key and the records gone;
// the next PIN makes a new store. Nothing logged: the log is on the console.
void WipeStore() {
    g_records->Clear();
    ForgetKey();
    storefile::Forget(g_file);
    g_header = storefile::Header();
    g_state = State::kLocked;
    g_why = nullptr;
}

void Opened() {
    g_state = State::kOpen;
    g_why = nullptr;
    ESP_LOGI(TAG, "%s: version %u, %u record(s)", g_path, static_cast<unsigned>(g_header.version),
             static_cast<unsigned>(g_records->Count()));
}

enum class Read { kOk, kNew, kWrongKey, kRefused };

// The file with g_key (or, with `pin`, a key derived from it and the file's
// salt) into the records. kRefused: Refuse() done.
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
        Refuse("the store file has no PIN key (an earlier format)");
        return Read::kRefused;
    }
    if (pin != nullptr && Derive(pin, h.salt, h.kdf_iterations) != ESP_OK) {
        Refuse("key derivation failed");
        return Read::kRefused;
    }
    const esp_err_t err =
        crypto::Aes256GcmOpen(g_key, h.nonce, raw, h.size, g_body, h.length, g_plain);
    if (err == ESP_ERR_INVALID_CRC) {
        return Read::kWrongKey;  // a wrong PIN, or a damaged file
    }
    if (err != ESP_OK) {
        Refuse("decryption failed");
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

// Records -> file, with g_key.
esp_err_t Save() {
    const size_t n = g_records->Serialize(g_plain, g_plain_capacity);
    if (n == 0) {
        return ESP_ERR_NO_MEM;  // never: the buffer holds a full store
    }
    storefile::Header h;
    h.kind = storefile::Kind::kRecords;
    h.cipher = storefile::Cipher::kAesGcm;
    h.version = storefile::NextVersion(g_file, g_header.version);
    h.length = static_cast<uint32_t>(n);
    h.modified_utc = Now();
    crypto::Random(h.nonce, sizeof h.nonce);
    h.kdf_iterations = g_iterations;
    memcpy(h.salt, g_salt, sizeof h.salt);
    uint8_t aad[storefile::kHeaderSize];
    storefile::Encode(h, aad);
    esp_err_t err = crypto::Aes256GcmSeal(g_key, h.nonce, aad, sizeof aad, g_plain, n, g_body);
    Wipe(g_plain, n);
    if (err == ESP_OK) {
        err = storefile::Save(g_file, h, g_body, n + storefile::kTagSize);
    }
    if (err == ESP_OK) {
        g_header = h;
    }
    return err;
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
    if (ReadFile(nullptr) == Read::kWrongKey) {
        Refuse("the store file no longer decrypts");
    }
    return Error::kStorage;
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
    // The interim key of earlier firmware (before the PIN): gone.
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, "key") == ESP_OK) {
            nvs_commit(h);
            ESP_LOGW(TAG, "interim key erased");
        }
        nvs_close(h);
    }
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

void SetWipeHook(void (*on_wipe)()) { g_on_wipe = on_wipe; }

// Under the lock; the wipe hook runs after it (`*wiped`).
static Error UnlockLocked(const char *pin, bool *wiped) {
    if (IsWipePin(pin)) {
        // Erase, then answer exactly as a wrong PIN on a locked store would:
        // the same result, the same count, the same time (one derivation),
        // the same log line.
        WipeStore();
        *wiped = true;
        if (!PinEntryAllowed()) {
            return Error::kPinLocked;
        }
        DeriveNew(pin);
        ForgetKey();
        ESP_LOGW(TAG, "unlock: wrong PIN");
        return PinWrong();
    }
    if (!PinEntryAllowed()) {
        return Error::kPinLocked;
    }
    if (g_state == State::kOpen) {
        // Stays open either way.
        if (!PinMatches(pin)) {
            return PinWrong();
        }
        PinRight();
        return Error::kOk;
    }
    switch (ReadFile(pin)) {
        case Read::kOk:
            PinRight();
            Opened();
            return Error::kOk;
        case Read::kNew:
            // No store yet: this PIN makes it.
            if (DeriveNew(pin) != ESP_OK || Save() != ESP_OK) {
                ForgetKey();
                g_state = State::kLocked;
                return Error::kStorage;
            }
            PinRight();
            ESP_LOGW(TAG, "new store made with this PIN");
            Opened();
            return Error::kOk;
        case Read::kWrongKey:
            ForgetKey();
            g_state = State::kLocked;
            g_why = nullptr;
            ESP_LOGW(TAG, "unlock: wrong PIN");
            return PinWrong();
        default:
            return Error::kLocked;
    }
}

Error Unlock(const char *pin) {
    if (!ValidPin(pin)) {
        return Error::kBadPin;
    }
    if (g_state == State::kOff) {
        return Error::kLocked;
    }
    Error e;
    bool wiped = false;
    {
        Guard guard;
        e = UnlockLocked(pin, &wiped);
    }
    if (wiped && g_on_wipe != nullptr) {
        g_on_wipe();
    }
    return e;
}

void Lock() {
    if (g_state == State::kOff) {
        return;
    }
    Guard guard;
    g_records->Clear();
    ForgetKey();
    g_state = State::kLocked;
    g_why = nullptr;
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
    uint8_t open_key[kKeySize];
    uint8_t open_salt[storefile::kSaltSize];
    const uint32_t open_iterations = g_iterations;
    memcpy(open_key, g_key, sizeof open_key);
    memcpy(open_salt, g_salt, sizeof open_salt);
    const bool ok = DeriveNew(new_pin) == ESP_OK && Save() == ESP_OK;
    if (!ok) {
        memcpy(g_key, open_key, sizeof g_key);
        memcpy(g_salt, open_salt, sizeof g_salt);
        g_iterations = open_iterations;
    } else {
        ESP_LOGW(TAG, "PIN changed");
    }
    Wipe(open_key, sizeof open_key);
    return ok ? Error::kOk : Error::kStorage;
}

Error MakeWipe(const char *pin, char out[kWipeTextSize]) {
    if (!ValidPin(pin)) {
        return Error::kBadPin;
    }
    if (g_state != State::kOpen) {
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
    info.kdf_iterations = g_state == State::kOpen ? g_iterations : 0;
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
    if (pin == nullptr ? g_state != State::kOpen : !ValidPin(pin)) {
        return pin == nullptr ? Error::kLocked : Error::kBadPin;
    }
    Guard guard;
    if (pin != nullptr && IsWipePin(pin)) {
        return Error::kPinInUse;
    }
    if (pin != nullptr && DeriveNew(pin) != ESP_OK) {
        return Error::kStorage;
    }
    g_records->Clear();
    if (Save() != ESP_OK) {
        ForgetKey();
        g_state = State::kLocked;
        return Error::kStorage;
    }
    ESP_LOGW(TAG, "%s reset: every record deleted", g_path);
    Opened();
    return Error::kOk;
}

}  // namespace vault
