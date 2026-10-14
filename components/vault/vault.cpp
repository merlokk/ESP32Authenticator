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

Error Unlock(const char *pin) {
    if (!ValidPin(pin)) {
        return Error::kBadPin;
    }
    if (g_state == State::kOff) {
        return Error::kLocked;
    }
    Guard guard;
    if (g_state == State::kOpen) {
        return PinMatches(pin) ? Error::kOk : Error::kWrongPin;  // stays open either way
    }
    switch (ReadFile(pin)) {
        case Read::kOk:
            Opened();
            return Error::kOk;
        case Read::kNew:
            // No store yet: this PIN makes it.
            if (DeriveNew(pin) != ESP_OK || Save() != ESP_OK) {
                ForgetKey();
                g_state = State::kLocked;
                return Error::kStorage;
            }
            ESP_LOGW(TAG, "new store made with this PIN");
            Opened();
            return Error::kOk;
        case Read::kWrongKey:
            ForgetKey();
            g_state = State::kLocked;
            g_why = nullptr;
            ESP_LOGW(TAG, "unlock: wrong PIN");
            return Error::kWrongPin;
        default:
            return Error::kLocked;
    }
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
    if (!PinMatches(old_pin)) {
        return Error::kWrongPin;
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
