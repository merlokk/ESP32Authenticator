#include "server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "crypto.h"
#include "esp_crt_bundle.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "rng.h"

namespace server {

namespace {

constexpr const char *TAG = "server";
constexpr size_t kHostSize = 64;   // config::kHostSize
constexpr size_t kEmailSize = 65;  // config::kEmailSize
constexpr size_t kPathSize = 48;
// TLS (mbedTLS) and the software P-256 want room: the task is made per request.
constexpr uint32_t kTaskStack = 10 * 1024;
constexpr int kTimeoutMs = 15000;
constexpr psa_algorithm_t kAlg = PSA_ALG_ECDSA(PSA_ALG_SHA_256);

SemaphoreHandle_t lock = nullptr;
StaticSemaphore_t lock_storage;
State state = {};
Status status = {};
char path[kPathSize] = "";
char temp_path[kPathSize + 4] = "";

struct Job {
    bool register_device;
    char host[kHostSize];
    char email[kEmailSize];
};

void Wipe(void *p, size_t n) {
    volatile auto *b = static_cast<volatile uint8_t *>(p);
    while (n-- > 0) {
        *b++ = 0;
    }
}

class Locked {
   public:
    Locked() { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Locked() { xSemaphoreGive(lock); }
};

// The last registration failed: status.said keeps why until the next one.
bool refused = false;

// A registration that went no further.
void Say(const char *text) {
    refused = true;
    Locked l;
    snprintf(status.said, sizeof status.said, "%s", text);
}

// server.bin.new, then remove + rename (SPIFFS rename does not replace).
esp_err_t Save(const State &s) {
    uint8_t bytes[kFileSize];
    Encode(s, bytes);
    FILE *f = fopen(temp_path, "wb");
    if (f == nullptr) {
        Wipe(bytes, sizeof bytes);
        return ESP_FAIL;
    }
    const bool written = fwrite(bytes, 1, sizeof bytes, f) == sizeof bytes;
    Wipe(bytes, sizeof bytes);
    if (fclose(f) != 0 || !written) {
        remove(temp_path);
        return ESP_FAIL;
    }
    remove(path);
    return rename(temp_path, path) == 0 ? ESP_OK : ESP_FAIL;
}

psa_key_id_t ImportKey(const uint8_t key[kKeySize]) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, kAlg);
    psa_key_id_t id = 0;
    return psa_import_key(&attr, key, kKeySize, &id) == PSA_SUCCESS ? id : 0;
}

// A private scalar from the hardware RNG: one in 1..n-1 (PSA refuses the
// rest, about 2^-32 of them; drawn again).
psa_key_id_t NewKey(uint8_t key[kKeySize]) {
    for (int tries = 0; tries < 8; ++tries) {
        rng::Fill(key, kKeySize);
        const psa_key_id_t id = ImportKey(key);
        if (id != 0) {
            return id;
        }
    }
    Wipe(key, kKeySize);
    return 0;
}

bool ExportPublic(psa_key_id_t id, char *out, size_t size) {
    uint8_t pub[kPublicSize];
    size_t n = 0;
    return psa_export_public_key(id, pub, sizeof pub, &n) == PSA_SUCCESS && n == kPublicSize &&
           Base64Url(pub, sizeof pub, out, size) != 0;
}

// The eFuse unique id as hex; the base MAC's when it cannot be read.
void Serial(char out[2 * kSerialSize + 1]) {
    uint8_t uid[kSerialSize] = {};
    if (esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, sizeof uid * 8) != ESP_OK) {
        esp_read_mac(uid, ESP_MAC_BASE);
    }
    Hex(uid, sizeof uid, out);
}

void Mac(char out[18]) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

esp_err_t last_error = ESP_OK;  // the last request's, for what to say

// What came back, kept up to its size (the rest is dropped).
struct Body {
    char *data;
    size_t size;
    size_t length;
};

esp_err_t OnEvent(esp_http_client_event_t *e) {
    auto *b = static_cast<Body *>(e->user_data);
    if (e->event_id == HTTP_EVENT_ON_DATA && b != nullptr && e->data_len > 0) {
        const size_t room = b->size - b->length;
        const size_t n = static_cast<size_t>(e->data_len) < room ? e->data_len : room;
        memcpy(b->data + b->length, e->data, n);
        b->length += n;
    }
    return ESP_OK;
}

// The HTTP status of a GET, or of a POST of `body` (a JWT: it carries the
// TOTP secret, never logged); -1 with no answer. `answer`: the body that
// came back, when wanted.
int Request(const char *url, const char *body = nullptr, size_t body_len = 0,
            Body *answer = nullptr) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = body != nullptr ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = kTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    cfg.event_handler = OnEvent;
    cfg.user_data = answer;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        last_error = ESP_ERR_NO_MEM;
        return -1;
    }
    if (body != nullptr) {
        esp_http_client_set_header(client, "Content-Type", "application/jwt");
        esp_http_client_set_post_field(client, body, static_cast<int>(body_len));
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int code = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    last_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return code;
}

void Answered(int code) {
    Locked l;
    status.http_status = code > 0 ? code : 0;
    status.link = code > 0 ? Link::kOk : Link::kFailed;
}

// The token, signed by the new key, into `token` (kTokenSize); its length,
// 0 when it fails.
size_t BuildToken(const Job &r, const State &s, psa_key_id_t id, char *token) {
    char serial[2 * kSerialSize + 1];
    char mac[18];
    char totp[Base32Length(kTotpSize) + 1];
    char pub[Base64UrlLength(kPublicSize) + 1];
    Serial(serial);
    Mac(mac);
    Base32(s.totp, kTotpSize, totp, sizeof totp);
    if (!ExportPublic(id, pub, sizeof pub)) {
        Wipe(totp, sizeof totp);
        return 0;
    }
    char *payload = static_cast<char *>(malloc(kPayloadSize));
    const Claims claims = {r.email, serial, mac, totp, pub, static_cast<int64_t>(time(nullptr))};
    const size_t payload_len = payload != nullptr ? Payload(claims, payload, kPayloadSize) : 0;
    const size_t input_len = payload_len != 0 ? SigningInput(payload, payload_len, token, kTokenSize) : 0;
    uint8_t signature[kSignatureSize];
    size_t signature_len = 0;
    const size_t length =
        input_len != 0 &&
                psa_sign_message(id, kAlg, reinterpret_cast<const uint8_t *>(token), input_len,
                                 signature, sizeof signature, &signature_len) == PSA_SUCCESS &&
                signature_len == kSignatureSize
            ? AppendSignature(token, input_len, kTokenSize, signature)
            : 0;
    Wipe(totp, sizeof totp);
    if (payload != nullptr) {
        Wipe(payload, kPayloadSize);
        free(payload);
    }
    return length;
}

void Register(const Job &r) {
    State s = {};
    const psa_key_id_t id = NewKey(s.key);
    if (id == 0) {
        Say("cannot make a key");
        return;
    }
    rng::Fill(s.totp, kTotpSize);
    s.has_key = true;
    // The old key and secret go now: a failed request leaves the device not
    // registered, with the new ones.
    const esp_err_t saved = Save(s);
    {
        Locked l;
        state = s;
        status.has_key = true;
        status.registered = false;
        status.registered_at = 0;
        status.last_sync = 0;
    }
    if (saved != ESP_OK) {
        psa_destroy_key(id);
        Wipe(&s, sizeof s);
        Say("server.bin is not written");
        return;
    }
    char url[kHostSize + 24];
    char *token = static_cast<char *>(malloc(kTokenSize));
    const size_t length = token != nullptr ? BuildToken(r, s, id, token) : 0;
    psa_destroy_key(id);
    if (length == 0 || !RegisterUrl(r.host, url, sizeof url)) {
        if (token != nullptr) {
            Wipe(token, kTokenSize);
            free(token);
        }
        Wipe(&s, sizeof s);
        Say("cannot sign the token");
        return;
    }
    const int code = Request(url, token, length);
    refused = code < 200 || code >= 300;
    Wipe(token, kTokenSize);
    free(token);
    Answered(code);
    if (code >= 200 && code < 300) {
        const int64_t now = time(nullptr);
        s.registered = true;
        s.registered_at = now;
        s.last_sync = now;
        Save(s);
        Locked l;
        state = s;
        status.registered = true;
        status.registered_at = now;
        status.last_sync = now;
        snprintf(status.said, sizeof status.said, "registered");
    } else if (code > 0) {
        Locked l;
        snprintf(status.said, sizeof status.said, "the server said %d", code);
    } else {
        Locked l;
        snprintf(status.said, sizeof status.said, "no answer: %s", esp_err_to_name(last_error));
    }
    ESP_LOGI(TAG, "register: %d", code);
    Wipe(&s, sizeof s);
}

// GET /api/device/ping: 200 {"status":"ok","time":N} is the server (and
// its clock); any other answer, or none, is "no server".
void Check(const Job &r) {
    char url[kHostSize + 24];
    char data[kPingBodySize];
    Body body = {data, sizeof data, 0};
    const int code = PingUrl(r.host, url, sizeof url) ? Request(url, nullptr, 0, &body) : -1;
    const int64_t now = time(nullptr);
    int64_t server_time = 0;
    const bool ok = code == 200 && ParsePing(data, body.length, &server_time);
    Locked l;
    status.http_status = code > 0 ? code : 0;
    status.link = ok ? Link::kOk : Link::kFailed;
    status.has_offset = ok;
    status.offset = ok ? now - server_time : 0;
    if (ok && state.registered) {
        state.last_sync = now;
        status.last_sync = now;
        Save(state);
    }
    if (ok) {
        if (state.registered || !refused) {
            snprintf(status.said, sizeof status.said, "the server answered");
        }
    } else if (code > 0) {
        snprintf(status.said, sizeof status.said, "not the server: HTTP %d", code);
    } else {
        snprintf(status.said, sizeof status.said, "no answer: %s", esp_err_to_name(last_error));
    }
    ESP_LOGI(TAG, "ping: %d, the device's clock %+lld s", code,
             static_cast<long long>(status.offset));
}

void Task(void *arg) {
    auto *r = static_cast<Job *>(arg);
    if (r->register_device) {
        Register(*r);
    } else {
        Check(*r);
    }
    Wipe(r, sizeof *r);
    free(r);
    {
        Locked l;
        status.busy = false;
    }
    vTaskDelete(nullptr);
}

esp_err_t Start(bool register_device, const char *host, const char *email) {
    if (lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (host == nullptr || host[0] == '\0' || strlen(host) >= kHostSize ||
        (register_device && (email == nullptr || email[0] == '\0' || strlen(email) >= kEmailSize))) {
        return ESP_ERR_INVALID_ARG;
    }
    {
        Locked l;
        if (status.busy) {
            return ESP_ERR_INVALID_STATE;
        }
        status.busy = true;
        snprintf(status.said, sizeof status.said, "%s",
                 register_device ? "registering..." : "connecting...");
    }
    auto *r = static_cast<Job *>(calloc(1, sizeof(Job)));
    if (r != nullptr) {
        r->register_device = register_device;
        snprintf(r->host, sizeof r->host, "%s", host);
        if (register_device) {
            snprintf(r->email, sizeof r->email, "%s", email);
        }
    }
    if (r == nullptr || xTaskCreate(Task, "server", kTaskStack, r, 4, nullptr) != pdPASS) {
        free(r);
        Locked l;
        status.busy = false;
        snprintf(status.said, sizeof status.said, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

}  // namespace

esp_err_t Init(const char *dir) {
    if (lock == nullptr) {
        lock = xSemaphoreCreateMutexStatic(&lock_storage);
    }
    psa_crypto_init();
    snprintf(path, sizeof path, "%s/server.bin", dir);
    snprintf(temp_path, sizeof temp_path, "%s.new", path);
    FILE *f = fopen(path, "rb");
    if (f == nullptr && rename(temp_path, path) == 0) {  // a save cut before its rename
        f = fopen(path, "rb");
    }
    if (f == nullptr) {
        return ESP_OK;  // not registered
    }
    uint8_t bytes[kFileSize + 1];
    const size_t n = fread(bytes, 1, sizeof bytes, f);
    fclose(f);
    State s = {};
    const bool ok = Decode(bytes, n, &s);
    Wipe(bytes, sizeof bytes);
    if (!ok) {
        ESP_LOGW(TAG, "%s is not a server file: not registered", path);
        return ESP_ERR_INVALID_STATE;
    }
    Locked l;
    state = s;
    Wipe(&s, sizeof s);
    status.has_key = state.has_key;
    status.registered = state.registered;
    status.registered_at = state.registered_at;
    status.last_sync = state.last_sync;
    return ESP_OK;
}

Status Get() {
    if (lock == nullptr) {
        return {};
    }
    Locked l;
    return status;
}

bool Code(int64_t unix_seconds, uint32_t *code) {
    if (lock == nullptr) {
        return false;
    }
    uint8_t totp[kTotpSize];
    {
        Locked l;
        if (!state.registered) {
            return false;
        }
        memcpy(totp, state.totp, sizeof totp);
    }
    const bool ok = crypto::Totp(totp, sizeof totp, unix_seconds, crypto::kTotpStep,
                                 crypto::kOtpDigits, code) == ESP_OK;
    Wipe(totp, sizeof totp);
    return ok;
}

esp_err_t StartRegister(const char *host, const char *email) { return Start(true, host, email); }

esp_err_t StartCheck(const char *host) { return Start(false, host, nullptr); }

bool PublicKey(char *out, size_t size) {
    if (lock == nullptr) {
        return false;
    }
    uint8_t key[kKeySize];
    {
        Locked l;
        if (!state.has_key) {
            return false;
        }
        memcpy(key, state.key, sizeof key);
    }
    const psa_key_id_t id = ImportKey(key);
    Wipe(key, sizeof key);
    const bool ok = id != 0 && ExportPublic(id, out, size);
    if (id != 0) {
        psa_destroy_key(id);
    }
    return ok;
}

}  // namespace server
