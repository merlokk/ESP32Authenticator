#include "server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "crypto.h"
#include "esp_crt_bundle.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
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
// The access token (a JWT, about 1 KB from the server) and an answer's body,
// in PSRAM.
constexpr size_t kAccessSize = 4096;
constexpr size_t kAnswerSize = kAccessSize + 512;
constexpr size_t kUrlSize = kHostSize + 32;
// The scheduler: a look every kTickMs.
constexpr uint32_t kTickMs = 5000;
constexpr uint32_t kSchedulerStack = 4096;

SemaphoreHandle_t lock = nullptr;
StaticSemaphore_t lock_storage;
State state = {};
Status status = {};
char path[kPathSize] = "";
char temp_path[kPathSize + 4] = "";
// The access token (under lock); empty: not signed in.
char *access = nullptr;
int64_t access_until = 0;  // monotonic seconds
// The status schedule (under lock): monotonic seconds, -1: none.
int64_t status_tried = -1;
int64_t status_sent = -1;

enum class Kind : uint8_t { kCheck, kRegister, kStatus };

struct Job {
    Kind kind;
    char host[kHostSize];
    char email[kEmailSize];
    Report report;  // kStatus; its texts static
};

void Wipe(void *p, size_t n) {
    volatile auto *b = static_cast<volatile uint8_t *>(p);
    while (n-- > 0) {
        *b++ = 0;
    }
}

int64_t Uptime() { return esp_timer_get_time() / 1000000; }

void *Psram(size_t n) {
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

class Locked {
   public:
    Locked() { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Locked() { xSemaphoreGive(lock); }
};

// The last registration failed: status.said keeps why until the next one.
bool refused = false;

void Said(const char *text) {
    Locked l;
    snprintf(status.said, sizeof status.said, "%s", text);
}

// A registration that went no further.
void Say(const char *text) {
    refused = true;
    Said(text);
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

// header.payload signed by `id` into `token` (kTokenSize): the whole JWT's
// length, 0 when it fails.
size_t Sign(psa_key_id_t id, const char *header, const char *payload, size_t payload_len,
            char *token) {
    const size_t input_len =
        SigningInput(header, strlen(header), payload, payload_len, token, kTokenSize);
    uint8_t signature[kSignatureSize];
    size_t signature_len = 0;
    return input_len != 0 &&
                   psa_sign_message(id, kAlg, reinterpret_cast<const uint8_t *>(token), input_len,
                                    signature, sizeof signature, &signature_len) == PSA_SUCCESS &&
                   signature_len == kSignatureSize
               ? AppendSignature(token, input_len, kTokenSize, signature)
               : 0;
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

// One request. A body (`type` its Content-Type) makes it a POST; `bearer`
// the access token. The bodies carry secrets (the JWTs, the token): never
// logged.
struct Call {
    const char *url;
    const char *type = nullptr;
    const char *body = nullptr;
    size_t body_len = 0;
    const char *bearer = nullptr;
    Body *answer = nullptr;
};

// The HTTP status; -1 with no answer.
int Request(const Call &c) {
    esp_http_client_config_t cfg = {};
    cfg.url = c.url;
    cfg.method = c.body != nullptr ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = kTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    cfg.event_handler = OnEvent;
    cfg.user_data = c.answer;
    // The request's head goes out of one buffer: the Authorization header
    // carries the whole token.
    cfg.buffer_size_tx = c.bearer != nullptr ? static_cast<int>(strlen(c.bearer)) + 1024 : 0;
    if (c.answer != nullptr) {
        c.answer->length = 0;
    }
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        last_error = ESP_ERR_NO_MEM;
        return -1;
    }
    char *auth = nullptr;
    if (c.bearer != nullptr) {
        const size_t n = strlen(c.bearer) + 8;
        auth = static_cast<char *>(Psram(n));
        if (auth == nullptr) {
            esp_http_client_cleanup(client);
            last_error = ESP_ERR_NO_MEM;
            return -1;
        }
        snprintf(auth, n, "Bearer %s", c.bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    if (c.body != nullptr) {
        esp_http_client_set_header(client, "Content-Type", c.type);
        esp_http_client_set_post_field(client, c.body, static_cast<int>(c.body_len));
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int code = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    last_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    if (auth != nullptr) {
        Wipe(auth, strlen(auth));
        free(auth);
    }
    return code;
}

// What a request that went wrong says: no answer, or the server's code.
void SayFailed(int code, const Body &answer, char *out, size_t size) {
    if (code <= 0) {
        snprintf(out, size, "no answer: %s", esp_err_to_name(last_error));
        return;
    }
    char error[48] = "";
    if (!ErrorCode(answer.data, answer.length, error, sizeof error)) {
        error[0] = '\0';
    }
    RefusalText(code, error, out, size);
}

void Answered(int code) {
    Locked l;
    status.http_status = code > 0 ? code : 0;
    status.link = code > 0 ? Link::kOk : Link::kFailed;
}

// The registration token, signed by the new key, into `token` (kTokenSize);
// its length, 0 when it fails.
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
    const size_t length = payload_len != 0 ? Sign(id, kHeader, payload, payload_len, token) : 0;
    Wipe(totp, sizeof totp);
    if (payload != nullptr) {
        Wipe(payload, kPayloadSize);
        free(payload);
    }
    return length;
}

void DropToken() {
    Locked l;
    if (access != nullptr) {
        Wipe(access, kAccessSize);
    }
    access_until = 0;
}

// POST /register: 200 {"deviceId","clientId"} (kept: the sign-in's).
void Register(const Job &r, Body &answer) {
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
    DropToken();
    {
        Locked l;
        state = s;
        status.has_key = true;
        status.registered = false;
        status.has_client_id = false;
        status.registered_at = 0;
        status.last_sync = 0;
        status.update[0] = '\0';
        status_tried = -1;
        status_sent = -1;
    }
    if (saved != ESP_OK) {
        psa_destroy_key(id);
        Wipe(&s, sizeof s);
        Say("server.bin is not written");
        return;
    }
    char url[kUrlSize];
    char *token = static_cast<char *>(malloc(kTokenSize));
    const size_t length = token != nullptr ? BuildToken(r, s, id, token) : 0;
    psa_destroy_key(id);
    if (length == 0 || !Url(r.host, "/register", url, sizeof url)) {
        if (token != nullptr) {
            Wipe(token, kTokenSize);
            free(token);
        }
        Wipe(&s, sizeof s);
        Say("cannot sign the token");
        return;
    }
    Call call = {url};
    call.type = "application/jwt";
    call.body = token;
    call.body_len = length;
    call.answer = &answer;
    const int code = Request(call);
    refused = code < 200 || code >= 300;
    Wipe(token, kTokenSize);
    free(token);
    Answered(code);
    if (!refused) {
        const int64_t now = time(nullptr);
        const int n = GetString(answer.data, answer.length, "clientId", s.client_id,
                                sizeof s.client_id);
        if (n < 0 || !ValidClientId(s.client_id, static_cast<size_t>(n))) {
            s.client_id[0] = '\0';
        }
        s.registered = true;
        s.registered_at = now;
        Save(s);
        Locked l;
        state = s;
        status.registered = true;
        status.has_client_id = s.client_id[0] != '\0';
        status.registered_at = now;
        snprintf(status.said, sizeof status.said, "%s",
                 status.has_client_id ? "registered" : "registered, no client id");
    } else {
        char said[sizeof status.said];
        SayFailed(code, answer, said, sizeof said);
        Said(said);
    }
    ESP_LOGI(TAG, "register: %d", code);
    Wipe(&s, sizeof s);
}

// POST /connect/token with a client assertion signed by the device's key;
// the access token kept (in PSRAM) until expires_in. False: why is in
// `said`.
bool SignIn(const char *host, Body &answer, char *said, size_t size) {
    char client_id[kClientIdSize + 1];
    uint8_t key[kKeySize];
    {
        Locked l;
        memcpy(client_id, state.client_id, sizeof client_id);
        memcpy(key, state.key, sizeof key);
    }
    if (client_id[0] == '\0') {
        Wipe(key, sizeof key);
        snprintf(said, size, "no client id: register again");
        return false;
    }
    const psa_key_id_t id = ImportKey(key);
    Wipe(key, sizeof key);
    uint8_t random[16];
    rng::Fill(random, sizeof random);
    char jti[2 * sizeof random + 1];
    Hex(random, sizeof random, jti);
    char payload[384];  // 2 x 36 + the host (63) + 32 + the names and numbers
    const size_t payload_len = AssertionPayload(client_id, host, jti, time(nullptr), payload,
                                                sizeof payload);
    char *assertion = static_cast<char *>(malloc(kTokenSize));
    const size_t length = id != 0 && assertion != nullptr && payload_len != 0
                              ? Sign(id, kAssertionHeader, payload, payload_len, assertion)
                              : 0;
    if (id != 0) {
        psa_destroy_key(id);
    }
    const size_t form_size = kTokenSize + 256;
    char *form = length != 0 ? static_cast<char *>(malloc(form_size)) : nullptr;
    const size_t form_len = form != nullptr ? TokenForm(client_id, assertion, form, form_size) : 0;
    free(assertion);
    char url[kUrlSize];
    if (form_len == 0 || !Url(host, "/connect/token", url, sizeof url)) {
        free(form);
        snprintf(said, size, "cannot sign the assertion");
        return false;
    }
    Call call = {url};
    call.type = "application/x-www-form-urlencoded";
    call.body = form;
    call.body_len = form_len;
    call.answer = &answer;
    const int code = Request(call);
    free(form);
    Answered(code);
    if (code != 200) {
        SayFailed(code, answer, said, size);
        ESP_LOGI(TAG, "sign-in: %d", code);
        return false;
    }
    if (access == nullptr) {
        access = static_cast<char *>(Psram(kAccessSize));
    }
    int64_t expires_in = 0;
    Locked l;
    if (access == nullptr ||
        !ParseToken(answer.data, answer.length, access, kAccessSize, &expires_in)) {
        if (access != nullptr) {
            Wipe(access, kAccessSize);
        }
        access_until = 0;
        snprintf(said, size, "sign-in: not a token");
        return false;
    }
    access_until = Uptime() + expires_in;
    ESP_LOGI(TAG, "signed in for %lld s", static_cast<long long>(expires_in));
    return true;
}

// POST /api/device/status (signed in first when the token is out); a 401
// signs in again once. The answer: the server's clock and the update.
void SendStatus(const Job &r, Body &answer) {
    char mac[18];
    Mac(mac);
    Report report = r.report;
    report.mac = mac;
    char body[kStatusBodySize];
    const size_t body_len = StatusBody(report, body, sizeof body);
    char url[kUrlSize];
    char said[sizeof status.said] = "";
    bool ok = body_len != 0 && Url(r.host, "/api/device/status", url, sizeof url);
    int code = -1;
    for (int attempt = 0; ok && attempt < 2; ++attempt) {
        bool fresh;
        {
            Locked l;
            fresh = access != nullptr && access[0] != '\0' &&
                    Uptime() < access_until - kTokenMargin;
        }
        if (!fresh && !SignIn(r.host, answer, said, sizeof said)) {
            ok = false;
            break;
        }
        Call call = {url};
        call.type = "application/json";
        call.body = body;
        call.body_len = body_len;
        call.bearer = access;  // the job task alone writes it
        call.answer = &answer;
        code = Request(call);
        Answered(code);
        if (code != 401) {
            break;
        }
        DropToken();  // expired or revoked: once more, signed in again
    }
    const int64_t now = time(nullptr);
    char update[kVersionSize] = "";
    int64_t server_time = 0;
    const bool taken =
        ok && code == 200 && ParseStatus(answer.data, answer.length, update, sizeof update, &server_time);
    if (ok && !taken) {
        if (code == 200) {
            snprintf(said, sizeof said, "status: not the server's answer");
        } else {
            SayFailed(code, answer, said, sizeof said);
        }
    }
    Locked l;
    if (taken) {
        status_sent = status_tried;
        state.last_sync = now;
        status.last_sync = now;
        Save(state);
        status.has_offset = true;
        status.offset = now - server_time;
        snprintf(status.update, sizeof status.update, "%s", update);
        snprintf(said, sizeof said, "status sent");
    }
    if (taken || !refused) {
        snprintf(status.said, sizeof status.said, "%s", said);
    }
    ESP_LOGI(TAG, "status: %d%s%s", code, update[0] != '\0' ? ", update " : "", update);
}

// GET /api/device/ping: 200 {"status":"ok","time":N} is the server (and
// its clock); any other answer, or none, is "no server".
void Check(const Job &r, Body &answer) {
    char url[kUrlSize];
    Call call = {url};
    call.answer = &answer;
    const int code = PingUrl(r.host, url, sizeof url) ? Request(call) : -1;
    const int64_t now = time(nullptr);
    int64_t server_time = 0;
    const bool ok = code == 200 && ParsePing(answer.data, answer.length, &server_time);
    Locked l;
    status.http_status = code > 0 ? code : 0;
    status.link = ok ? Link::kOk : Link::kFailed;
    status.has_offset = ok;
    status.offset = ok ? now - server_time : 0;
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
    Body answer = {static_cast<char *>(Psram(kAnswerSize)), kAnswerSize, 0};
    if (answer.data == nullptr) {
        Said("out of memory");
    } else if (r->kind == Kind::kRegister) {
        Register(*r, answer);
    } else if (r->kind == Kind::kStatus) {
        SendStatus(*r, answer);
    } else {
        Check(*r, answer);
    }
    if (answer.data != nullptr) {
        Wipe(answer.data, kAnswerSize);  // a token may be in it
        free(answer.data);
    }
    Wipe(r, sizeof *r);
    free(r);
    {
        Locked l;
        status.busy = false;
    }
    vTaskDelete(nullptr);
}

esp_err_t Start(Kind kind, const char *host, const char *email, const Report *report) {
    if (lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool registering = kind == Kind::kRegister;
    if (host == nullptr || host[0] == '\0' || strlen(host) >= kHostSize ||
        (registering && (email == nullptr || email[0] == '\0' || strlen(email) >= kEmailSize))) {
        return ESP_ERR_INVALID_ARG;
    }
    {
        Locked l;
        if (status.busy) {
            return ESP_ERR_INVALID_STATE;
        }
        if (kind == Kind::kStatus && !(state.registered && state.client_id[0] != '\0')) {
            return ESP_ERR_NOT_FOUND;
        }
        status.busy = true;
        if (kind == Kind::kStatus) {
            status_tried = Uptime();
        }
        if (kind != Kind::kStatus || !refused) {
            snprintf(status.said, sizeof status.said, "%s",
                     registering ? "registering..." : kind == Kind::kStatus ? "sending the status..."
                                                                            : "connecting...");
        }
    }
    auto *r = static_cast<Job *>(calloc(1, sizeof(Job)));
    if (r != nullptr) {
        r->kind = kind;
        snprintf(r->host, sizeof r->host, "%s", host);
        if (registering) {
            snprintf(r->email, sizeof r->email, "%s", email);
        }
        if (report != nullptr) {
            r->report = *report;
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

// --- The schedule ---------------------------------------------------------------

const Hooks *hooks = nullptr;
StaticTask_t scheduler_task;
StackType_t scheduler_stack[kSchedulerStack / sizeof(StackType_t)];

void Scheduler(void *) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
        bool registered;
        int64_t tried;
        int64_t sent;
        {
            Locked l;
            registered = state.registered && state.client_id[0] != '\0' && !status.busy;
            tried = status_tried;
            sent = status_sent;
        }
        const bool ready = registered && hooks->ready();
        if (StatusDue(ready, hooks->status_minutes(), Uptime(), tried, sent)) {
            StartStatus();
        }
    }
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
    status.has_client_id = state.client_id[0] != '\0';
    status.registered_at = state.registered_at;
    status.last_sync = state.last_sync;
    return ESP_OK;
}

void StartSchedule(const Hooks *h) {
    if (lock == nullptr || hooks != nullptr) {
        return;
    }
    hooks = h;
    xTaskCreateStatic(Scheduler, "server_due", kSchedulerStack / sizeof(StackType_t), nullptr, 2,
                      scheduler_stack, &scheduler_task);
}

Status Get() {
    if (lock == nullptr) {
        return {};
    }
    Locked l;
    Status s = status;
    const int64_t left = access_until - Uptime();
    s.token_left = access != nullptr && access[0] != '\0' && left > 0 ? left : 0;
    return s;
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

esp_err_t StartRegister(const char *host, const char *email) {
    return Start(Kind::kRegister, host, email, nullptr);
}

esp_err_t StartCheck(const char *host) { return Start(Kind::kCheck, host, nullptr, nullptr); }

esp_err_t StartStatus() {
    if (hooks == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    char host[kHostSize];
    hooks->host(host, sizeof host);
    Report report = {};
    hooks->report(&report);
    return Start(Kind::kStatus, host, nullptr, &report);
}

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

bool ClientId(char *out, size_t size) {
    if (lock == nullptr) {
        return false;
    }
    Locked l;
    if (state.client_id[0] == '\0') {
        return false;
    }
    snprintf(out, size, "%s", state.client_id);
    return true;
}

}  // namespace server
