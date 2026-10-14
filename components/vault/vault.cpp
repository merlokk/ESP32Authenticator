#include "vault.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#include "clock_math.h"
#include "crypto.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "store_io.h"

namespace vault {

namespace {

constexpr const char *TAG = "vault";
constexpr const char *kFileName = "vault.bin";
constexpr const char *kNvsNamespace = "vault";
constexpr const char *kNvsKey = "key";

StaticSemaphore_t g_lock_storage;
SemaphoreHandle_t g_lock = nullptr;

State g_state = State::kOff;
const char *g_why = nullptr;
char g_path[64];
storefile::File g_file = {g_path, storefile::Kind::kRecords, kNvsNamespace};
storefile::Header g_header;  // of the loaded / last written file

uint8_t g_key[kKeySize];
bool g_has_key = false;

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

void Refuse(const char *why) {
    g_records->Clear();
    g_state = State::kRefused;
    g_why = why;
    ESP_LOGE(TAG, "%s not loaded: %s", g_path, why);
}

// File -> records. Leaves kOpen or kRefused.
void Load() {
    g_records->Clear();
    g_header = storefile::Header();
    size_t body_size = 0;
    storefile::Header h;
    const char *why = nullptr;
    const storefile::Loaded loaded =
        storefile::Load(g_file, g_body, g_plain_capacity + storefile::kTagSize, &h, &body_size,
                        &why);
    if (loaded == storefile::Loaded::kRefused) {
        Refuse(why);
        return;
    }
    if (loaded == storefile::Loaded::kOk) {
        if (h.cipher != storefile::Cipher::kAesGcm) {
            Refuse("the store file is not encrypted");
            return;
        }
        uint8_t aad[storefile::kHeaderSize];
        storefile::Encode(h, aad);
        const esp_err_t err = crypto::Aes256GcmOpen(g_key, h.nonce, aad, sizeof aad, g_body,
                                                    h.length, g_plain);
        if (err != ESP_OK) {
            Refuse(err == ESP_ERR_INVALID_CRC
                       ? "the store file does not decrypt (other key, or damaged)"
                       : "decryption failed");
            return;
        }
        const Error e = g_records->Parse(g_plain, h.length);
        Wipe(g_plain, h.length);
        if (e != Error::kOk) {
            Refuse(e == Error::kFull ? "the store file holds more records than fit"
                                     : "the store file's records are malformed");
            return;
        }
        g_header = h;
    }
    g_state = State::kOpen;
    g_why = nullptr;
    ESP_LOGI(TAG, "%s: version %u, %u record(s)", g_path, static_cast<unsigned>(g_header.version),
             static_cast<unsigned>(g_records->Count()));
}

// Records -> file. On a failure the file is reloaded (the change undone).
Error Save() {
    const size_t n = g_records->Serialize(g_plain, g_plain_capacity);
    storefile::Header h;
    h.kind = storefile::Kind::kRecords;
    h.cipher = storefile::Cipher::kAesGcm;
    h.version = storefile::NextVersion(g_file, g_header.version);
    h.length = static_cast<uint32_t>(n);
    h.modified_utc = Now();
    crypto::Random(h.nonce, sizeof h.nonce);
    uint8_t aad[storefile::kHeaderSize];
    storefile::Encode(h, aad);
    esp_err_t err = n == 0 && g_records->Count() != 0 ? ESP_ERR_NO_MEM : ESP_OK;
    if (err == ESP_OK) {
        err = crypto::Aes256GcmSeal(g_key, h.nonce, aad, sizeof aad, g_plain, n, g_body);
    }
    Wipe(g_plain, n);
    if (err == ESP_OK) {
        err = storefile::Save(g_file, h, g_body, n + storefile::kTagSize);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed (%s): reloading", esp_err_to_name(err));
        Load();
        return Error::kStorage;
    }
    g_header = h;
    return Error::kOk;
}

Error Changed(Error e) { return e == Error::kOk ? Save() : e; }

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
    ESP_LOGI(TAG, "%s, up to %u records, %u bytes of PSRAM", g_path,
             static_cast<unsigned>(capacity),
             static_cast<unsigned>(capacity * sizeof(Record) + 2 * g_plain_capacity));
    return ESP_OK;
}

Error Unlock(const uint8_t key[kKeySize]) {
    if (g_state == State::kOff) {
        return Error::kLocked;
    }
    Guard guard;
    memcpy(g_key, key, kKeySize);
    g_has_key = true;
    Load();
    return g_state == State::kOpen ? Error::kOk : Error::kLocked;
}

void Lock() {
    if (g_state == State::kOff) {
        return;
    }
    Guard guard;
    g_records->Clear();
    Wipe(g_key, sizeof g_key);
    g_has_key = false;
    g_state = State::kLocked;
    g_why = nullptr;
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

Error Reset() {
    if (g_state == State::kOff || !g_has_key) {
        return Error::kLocked;
    }
    Guard guard;
    g_records->Clear();
    const Error e = Save();
    if (e == Error::kOk) {
        g_state = State::kOpen;
        g_why = nullptr;
        ESP_LOGW(TAG, "%s reset: every record deleted", g_path);
    }
    return e;
}

esp_err_t InterimKey(uint8_t out[kKeySize]) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = kKeySize;
    err = nvs_get_blob(h, kNvsKey, out, &size);
    if (err == ESP_OK && size != kKeySize) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        crypto::Random(out, kKeySize);
        err = nvs_set_blob(h, kNvsKey, out, kKeySize);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "interim key made (NVS)");
        }
    }
    nvs_close(h);
    if (err != ESP_OK) {
        Wipe(out, kKeySize);
    }
    return err;
}

}  // namespace vault
