#include "server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

#include "crypto.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
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
#include "nvs.h"
#include "ota_update.h"
#include "psa/crypto.h"
#include "rng.h"
#include "store_header.h"

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

enum class Kind : uint8_t { kCheck, kRegister, kStatus, kSync, kResolve, kFirmware, kInstall };

struct Job {
    Kind kind;
    char host[kHostSize];
    char email[kEmailSize];
    Report report;  // kStatus; its texts static
    sync::Store store;  // kResolve
    Choice choice;
    char pin[8];        // kResolve: the server's copy's PIN, "" none (wiped after)
    uint8_t pick;       // kResolve: which of the choices is taken
    bool by_hand;       // kSync: asked for (the other phrases' stores looked at)
    firmware::Release release;  // kInstall
};

const Hooks *hooks = nullptr;  // StartSchedule

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

// What came back, kept up to its size (the rest is dropped); or written to
// `file` (a store's copy), up to kMaxCopy.
struct Body {
    char *data;
    size_t size;
    size_t length;
    FILE *file = nullptr;
    bool failed = false;  // `file`: not written, or too long
};

constexpr size_t kMaxCopy = sync::kMaxFileBytes;

esp_err_t OnEvent(esp_http_client_event_t *e) {
    auto *b = static_cast<Body *>(e->user_data);
    if (e->event_id != HTTP_EVENT_ON_DATA || b == nullptr || e->data_len <= 0) {
        return ESP_OK;
    }
    const size_t len = static_cast<size_t>(e->data_len);
    if (b->file != nullptr) {
        b->length += len;
        b->failed |= b->length > kMaxCopy || fwrite(e->data, 1, len, b->file) != len;
        return ESP_OK;
    }
    const size_t room = b->size - b->length;
    const size_t n = len < room ? len : room;
    memcpy(b->data + b->length, e->data, n);
    b->length += n;
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

// The answer's HTTP status; -1: none. A 401 whose WWW-Authenticate is
// Bearer (a token the server no longer takes: its keys changed) fails
// esp_http_client_perform with ESP_ERR_NOT_SUPPORTED: still a 401.
int StatusOf(esp_http_client_handle_t client, esp_err_t err) {
    const int code = esp_http_client_get_status_code(client);
    return err == ESP_OK || (err == ESP_ERR_NOT_SUPPORTED && code == 401) ? code : -1;
}

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
    const int code = StatusOf(client, err);
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

bool Fresh() {
    Locked l;
    return access != nullptr && access[0] != '\0' && Uptime() < access_until - kTokenMargin;
}

// `call` with the access token, signed in first when it is out (`answer` is
// the sign-in's buffer); a 401 signs in again once. -2: no sign-in (`said`
// says why).
int SignedCall(Call call, const char *host, Body &answer, char *said, size_t size) {
    int code = 401;
    for (int attempt = 0; attempt < 2 && code == 401; ++attempt) {
        if (!Fresh() && !SignIn(host, answer, said, size)) {
            return -2;
        }
        call.bearer = access;  // the job task alone writes it
        code = Request(call);
        Answered(code);
        if (code == 401) {
            DropToken();  // expired or revoked: once more, signed in again
        }
    }
    return code;
}

// --- Firmware (firmware_logic.h) ----------------------------------------------------

constexpr size_t kFirmwareUrlSize = 8 + kHostSize + 80;
constexpr size_t kListSize = 16 * 1024;  // the list's answer (every release of the board)
constexpr int kMaxStalls = 5;            // pieces in a row that brought nothing
constexpr uint32_t kStallPauseMs = 2000;  // before asking for it again

// This board's releases, newest first (under lock); PSRAM, kMaxReleases.
firmware::Release *releases = nullptr;
size_t release_count = 0;

const char *Running() { return esp_app_get_description()->version; }

void FirmwareSaid(const char *text) {
    Locked l;
    snprintf(status.firmware_said, sizeof status.firmware_said, "%s", text);
}

// GET /api/device/firmware?board=: the releases kept, `newer` worked out.
void ListFirmware(const Job &r) {
    char url[kFirmwareUrlSize];
    if (r.report.board == nullptr || !firmware::ListUrl(r.host, r.report.board, url, sizeof url)) {
        FirmwareSaid("no board to ask for");
        return;
    }
    Body list = {static_cast<char *>(Psram(kListSize)), kListSize, 0};
    auto *found = static_cast<firmware::Release *>(
        Psram(firmware::kMaxReleases * sizeof(firmware::Release)));
    if (list.data == nullptr || found == nullptr) {
        free(list.data);
        free(found);
        FirmwareSaid("out of memory");
        return;
    }
    Call call = {url};
    call.answer = &list;
    char said[sizeof status.firmware_said] = "";
    const int code = SignedCall(call, r.host, list, said, sizeof said);
    const int n = code == 200 ? firmware::ParseReleases(list.data, list.length, found) : -1;
    if (code != 200 && code != -2) {
        SayFailed(code, list, said, sizeof said);
    } else if (code == 200 && n < 0) {
        snprintf(said, sizeof said, "firmware: not the server's list");
    }
    Wipe(list.data, kListSize);  // a sign-in's token may be in it
    free(list.data);
    {
        Locked l;
        if (n >= 0) {
            if (releases == nullptr) {
                releases = found;
                found = nullptr;
            } else {
                memcpy(releases, found, static_cast<size_t>(n) * sizeof(firmware::Release));
            }
            release_count = static_cast<size_t>(n);
            status.firmware_known = true;
            status.firmware_count = static_cast<uint8_t>(n);
            status.firmware_newer = firmware::AnyNewer(releases, release_count, Running());
            status.firmware_at = time(nullptr);
            snprintf(said, sizeof said, "%s", n == 0 ? "no releases for this board" : "");
        }
        snprintf(status.firmware_said, sizeof status.firmware_said, "%s", said);
    }
    free(found);
    ESP_LOGI(TAG, "firmware: %d, %d release(s)", code, n);
}

// One image download: the pieces go into the OTA slot as they come.
struct Download {
    psa_hash_operation_t hash;
    uint32_t offset;  // bytes written so far
    bool range_ok;    // this answer's Content-Range starts at `offset`
    bool failed;      // ota::Write refused (the update is over: ota says why)
    Body *refusal;    // a refusal's body
};

esp_err_t OnImage(esp_http_client_event_t *e) {
    auto *d = static_cast<Download *>(e->user_data);
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Content-Range") == 0) {
        uint32_t first = 0;
        d->range_ok = firmware::RangeStart(e->header_value, &first) && first == d->offset;
        return ESP_OK;
    }
    if (e->event_id != HTTP_EVENT_ON_DATA || e->data_len <= 0) {
        return ESP_OK;
    }
    const size_t len = static_cast<size_t>(e->data_len);
    if (esp_http_client_get_status_code(e->client) != 206) {
        Body *b = d->refusal;  // kept for what to say
        const size_t room = b->size - b->length;
        const size_t n = len < room ? len : room;
        memcpy(b->data + b->length, e->data, n);
        b->length += n;
        return ESP_OK;
    }
    if (!d->range_ok || d->failed) {
        return ESP_OK;  // not the bytes asked for: asked again
    }
    const auto *data = static_cast<const uint8_t *>(e->data);
    if (ota::Write(data, len) != ESP_OK) {
        d->failed = true;
        return ESP_OK;
    }
    psa_hash_update(&d->hash, data, len);
    d->offset += len;
    return ESP_OK;
}

// GET /api/device/firmware/{id}/image, a piece (kPieceSize) a request on one
// connection, each from where the slot stands (a cut piece costs only its
// rest); its SHA-256 the release's, then ota::End. "": installed; nullptr:
// ota failed it (its window says why); else why not.
const char *Fetch(const Job &r, Body &answer, char *said, size_t size) {
    const firmware::Release &rel = r.release;
    char url[kFirmwareUrlSize];
    if (!firmware::ImageUrl(r.host, rel.id, url, sizeof url)) {
        return "not a release id";
    }
    if (!Fresh() && !SignIn(r.host, answer, said, size)) {
        return said;
    }
    if (ota::Begin(ota::Source::kServer, rel.size) != ESP_OK) {
        ota::Status o;
        ota::GetStatus(&o);
        snprintf(said, size, "%s", o.error[0] != '\0' ? o.error : "cannot start an update");
        return said;
    }
    Download d = {};
    d.hash = psa_hash_operation_init();
    d.refusal = &answer;
    char *auth = static_cast<char *>(Psram(kAccessSize + 8));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = kTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    cfg.event_handler = OnImage;
    cfg.user_data = &d;
    cfg.buffer_size_tx = static_cast<int>(kAccessSize) + 1024;  // the Authorization header
    esp_http_client_handle_t client =
        auth != nullptr && psa_hash_setup(&d.hash, PSA_ALG_SHA_256) == PSA_SUCCESS
            ? esp_http_client_init(&cfg)
            : nullptr;
    const char *why = client == nullptr ? "out of memory" : nullptr;
    int stalls = 0;
    while (why == nullptr && !d.failed && d.offset < rel.size) {
        if (!Fresh() && !SignIn(r.host, answer, said, size)) {
            why = said;
            break;
        }
        {
            Locked l;
            snprintf(auth, kAccessSize + 8, "Bearer %s", access);
        }
        char range[40];
        firmware::RangeHeader(d.offset, rel.size, range, sizeof range);
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_header(client, "Range", range);
        const uint32_t before = d.offset;
        d.range_ok = false;
        answer.length = 0;
        const esp_err_t err = esp_http_client_perform(client);
        const int code = StatusOf(client, err);
        last_error = err;
        Answered(code);
        if (code == 401) {
            DropToken();  // signed in again for the next piece
        } else if (code == 404) {
            why = "the release is gone from the server";
        } else if (code > 0 && code != 206 && !firmware::Transient(code)) {
            SayFailed(code, answer, said, size);
            why = said;
        }
        stalls = d.offset > before ? 0 : stalls + 1;
        if (why == nullptr && stalls >= kMaxStalls) {
            if (code > 0 && code != 206) {
                SayFailed(code, answer, said, size);  // the last refusal, e.g. a 502
            } else {
                snprintf(said, size, "download stalled: %s",
                         code > 0 ? "no piece came" : esp_err_to_name(err));
            }
            why = said;
        } else if (why == nullptr && stalls > 0) {
            vTaskDelay(pdMS_TO_TICKS(kStallPauseMs));  // a busy proxy, a dropped link: again
        }
    }
    if (client != nullptr) {
        esp_http_client_cleanup(client);
    }
    if (auth != nullptr) {
        Wipe(auth, kAccessSize + 8);
        free(auth);
    }
    uint8_t digest[32] = {};
    size_t digest_len = 0;
    const bool hashed = psa_hash_finish(&d.hash, digest, sizeof digest, &digest_len) ==
                            PSA_SUCCESS &&
                        digest_len == sizeof digest;
    if (d.failed) {
        return nullptr;
    }
    char hex[firmware::kShaSize];
    Hex(digest, sizeof digest, hex);
    if (why == nullptr && (!hashed || strcmp(hex, rel.sha256) != 0)) {
        why = "the image's SHA-256 is not the release's";
    }
    if (why != nullptr) {
        ota::Abort(why);
        return why;
    }
    return ota::End() == ESP_OK ? "" : nullptr;
}

void Install(const Job &r, Body &answer) {
    char said[sizeof status.firmware_said] = "";
    ESP_LOGI(TAG, "firmware: installing %s (%u bytes)", r.release.version,
             static_cast<unsigned>(r.release.size));
    const char *why = Fetch(r, answer, said, sizeof said);
    if (why != nullptr && why[0] == '\0') {
        FirmwareSaid("installed: restarting");
        ota::Restart();  // the window said "Installed"
        return;
    }
    if (why == nullptr) {
        ota::Status o;
        ota::GetStatus(&o);
        why = o.error[0] != '\0' ? o.error : "not installed";
    }
    ESP_LOGW(TAG, "firmware: %s", why);
    FirmwareSaid(why);
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
    if (ok) {
        Call call = {url};
        call.type = "application/json";
        call.body = body;
        call.body_len = body_len;
        call.answer = &answer;
        code = SignedCall(call, r.host, answer, said, sizeof said);
        ok = code != -2;
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
    {
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
    }
    ESP_LOGI(TAG, "status: %d%s%s", code, update[0] != '\0' ? ", update " : "", update);
    if (taken) {
        ListFirmware(r);  // signed in already (not under the lock): for the newer one's icon
    }
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

// --- The stores' sync (sync_logic.h) -----------------------------------------------

constexpr const char *kSyncNvs = "srvsync";  // "s0", "s1": the synced copies
constexpr size_t kSyncUrlSize = 8 + kHostSize + 80;

// Under lock.
sync::Copy synced[sync::kStores] = {};
// The server's side of a store that waits for a person.
struct Waiting {
    sync::Copy server;
    bool other_store;
};
Waiting waiting[sync::kStores] = {};
// A conflict with several of the account's stores: the ones to pick from.
sync::Candidate choices[sync::kStores][sync::kMaxChoices] = {};
// The server's task's: a look's stores.
sync::Candidate looked[sync::kMaxChoices] = {};
int64_t sync_tried = -1;  // monotonic seconds, -1: none
int64_t sync_done = -1;
uint32_t seen_version[sync::kStores] = {};
bool seen_open = false;
int64_t others_at = -1;  // the other phrases' stores last looked at (monotonic), -1 never
constexpr int64_t kOthersSeconds = 10 * 60;

size_t Index(sync::Store s) { return static_cast<size_t>(s); }

void LoadSynced() {
    nvs_handle_t h;
    if (nvs_open(kSyncNvs, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < sync::kStores; ++i) {
        const char key[3] = {'s', static_cast<char>('0' + i), '\0'};
        uint8_t bytes[sync::kSyncedSize];
        size_t n = sizeof bytes;
        if (nvs_get_blob(h, key, bytes, &n) == ESP_OK &&
            (n == sizeof bytes || n == sync::kSyncedOldSize)) {
            Locked l;
            synced[i] = sync::DecodeSynced(bytes, n);
        }
    }
    nvs_close(h);
}

// The write both sides have now.
void SaveSynced(sync::Store s, const sync::Copy &c) {
    const size_t i = Index(s);
    {
        Locked l;
        if (sync::SameWrite(synced[i], c)) {
            return;
        }
        synced[i] = c;
    }
    uint8_t bytes[sync::kSyncedSize];
    sync::EncodeSynced(c, bytes);
    const char key[3] = {'s', static_cast<char>('0' + i), '\0'};
    nvs_handle_t h;
    esp_err_t err = nvs_open(kSyncNvs, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, bytes, sizeof bytes);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sync: the synced copy not kept: %s", esp_err_to_name(err));
    }
}

// Our file's header: the copy, its seed id and the file's size. False: no
// file here, or not an encrypted store.
bool ReadLocal(sync::Store s, sync::Copy *out, uint8_t seed_id[sync::kSeedIdSize],
               size_t *size) {
    *out = sync::Copy();
    const char *path = hooks->store_path(s);
    struct stat st;
    if (path == nullptr || stat(path, &st) != 0) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    uint8_t head[storefile::kHeaderSize];
    const size_t got = fread(head, 1, sizeof head, f);
    fclose(f);
    storefile::Header h;
    if (!storefile::Decode(head, got, static_cast<size_t>(st.st_size),
                           static_cast<storefile::Kind>(sync::KindOf(s)), &h) ||
        h.cipher != storefile::Cipher::kAesGcm) {
        return false;
    }
    out->has = true;
    memcpy(out->store_id, h.store_id, sizeof out->store_id);
    out->version = h.version;
    memcpy(out->nonce, h.nonce, sizeof out->nonce);
    memcpy(out->seed_id, h.seed_id, sizeof out->seed_id);
    memcpy(seed_id, h.seed_id, sync::kSeedIdSize);
    *size = static_cast<size_t>(st.st_size);
    return true;
}

void SetStore(sync::Store s, sync::Phase phase, uint32_t local, uint32_t server = 0,
              bool other = false, const char *at = "") {
    Locked l;
    StoreStatus &t = status.stores[Index(s)];
    t.phase = phase;
    t.local_version = local;
    t.server_version = server;
    t.other_store = other;
    snprintf(t.server_at, sizeof t.server_at, "%s", at);
}

// `picks` (`count` of them): the stores the person picks from, 0 none.
void SetWaiting(sync::Store s, const sync::Copy &server, bool other,
                const sync::Candidate *picks = nullptr, size_t count = 0) {
    Locked l;
    waiting[Index(s)] = {server, other};
    count = count > 1 && count <= sync::kMaxChoices ? count : 0;
    for (size_t i = 0; i < count; ++i) {
        choices[Index(s)][i] = picks[i];
    }
    status.stores[Index(s)].choices = static_cast<uint8_t>(count);
}

enum class Outcome : uint8_t { kInSync, kUploaded, kTaken, kWaits, kFailed };

// POST /api/device/stores/files: our file as it is (read whole into PSRAM,
// up to 256 KB). 200 kept; 409 not newer (`*theirs` the server's version).
int Upload(const char *host, const char *path, size_t size, Body &answer, char *said,
           size_t said_size, uint32_t *theirs) {
    if (size > sync::kMaxFileBytes) {
        snprintf(said, said_size, "too large for the server");
        return -3;
    }
    char *data = static_cast<char *>(Psram(size));
    FILE *f = data != nullptr ? fopen(path, "rb") : nullptr;
    const bool read = f != nullptr && fread(data, 1, size, f) == size;
    if (f != nullptr) {
        fclose(f);
    }
    char url[kSyncUrlSize];
    if (!read || !Url(host, "/api/device/stores/files", url, sizeof url)) {
        free(data);
        snprintf(said, said_size, "cannot read the store file");
        return -3;
    }
    Call call = {url};
    call.type = "application/octet-stream";
    call.body = data;
    call.body_len = size;
    call.answer = &answer;
    const int code = SignedCall(call, host, answer, said, said_size);
    free(data);
    if (code == 409) {
        int64_t v = 0;
        GetNumber(answer.data, answer.length, "version", &v);
        *theirs = static_cast<uint32_t>(v);
    } else if (code != 200 && code != -2) {
        SayFailed(code, answer, said, said_size);
    }
    return code;
}

// GET the server's copy into `to`: 200 there; 204 nothing above `above`;
// else `to` removed. A 401 signs in again once.
int Download(const char *host, const uint8_t store_id[sync::kStoreIdSize],
             const uint8_t seed_id[sync::kSeedIdSize], uint8_t kind, uint32_t above,
             const char *to, Body &answer, char *said, size_t size) {
    char url[kSyncUrlSize];
    if (!sync::DownloadUrl(host, store_id, seed_id, kind, above, url, sizeof url)) {
        snprintf(said, size, "the host is too long");
        return -3;
    }
    int code = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!Fresh() && !SignIn(host, answer, said, size)) {
            code = -2;
            break;
        }
        FILE *f = fopen(to, "wb");
        if (f == nullptr) {
            snprintf(said, size, "cannot write the copy");
            code = -3;
            break;
        }
        Body file = {nullptr, 0, 0};
        file.file = f;
        Call call = {url};
        call.bearer = access;
        call.answer = &file;
        code = Request(call);
        Answered(code);
        const bool written = fclose(f) == 0 && !file.failed;
        if (code == 200 && !written) {
            snprintf(said, size, "cannot write the copy (or over %u KB)",
                     static_cast<unsigned>(kMaxCopy / 1024));
            code = -3;
        }
        if (code != 401) {
            break;
        }
        DropToken();
    }
    if (code != 200) {
        remove(to);
    }
    if (code == -1) {
        SayFailed(code, answer, said, size);
    } else if (code > 0 && code != 200 && code != 204) {
        snprintf(said, size, "download: HTTP %d", code);
    }
    return code;
}

// Our copy up; 200: the synced one.
Outcome Send(const char *host, sync::Store s, const sync::Copy &local, size_t size, Body &answer,
             char *said, size_t said_size) {
    uint32_t theirs = 0;
    const int code = Upload(host, hooks->store_path(s), size, answer, said, said_size, &theirs);
    if (code == 200) {
        SaveSynced(s, local);
        SetStore(s, sync::Phase::kInSync, local.version);
        return Outcome::kUploaded;
    }
    if (code == 409) {
        // Another device got there first.
        sync::Copy was;
        {
            Locked l;
            was = synced[Index(s)];
        }
        if (sync::SameWrite(local, was)) {
            snprintf(said, said_size, "the server has a newer copy: again in a minute");
            SetStore(s, sync::Phase::kFailed, local.version);
            return Outcome::kFailed;
        }
        sync::Copy server = local;
        server.version = theirs;
        memset(server.nonce, 0, sizeof server.nonce);
        SetWaiting(s, server, false);
        SetStore(s, sync::Phase::kConflict, local.version, theirs);
        return Outcome::kWaits;
    }
    SetStore(s, sync::Phase::kFailed, local.version);
    return Outcome::kFailed;
}

// The server's copy of `store_id` down (above `above`) and taken (`force`:
// any version); `server` / `other`: what a PIN asked for waits with.
Outcome Take(const char *host, sync::Store s, const sync::Copy &local,
             const uint8_t seed_id[sync::kSeedIdSize], const sync::Copy &server, bool other,
             uint32_t above, bool force, const char *pin, Body &answer, char *said,
             size_t said_size) {
    char dl[kPathSize + 32];
    snprintf(dl, sizeof dl, "%s.dl", hooks->store_path(s));
    const int code =
        Download(host, server.store_id, seed_id, sync::KindOf(s), above, dl, answer, said, said_size);
    if (code == 204) {
        snprintf(said, said_size, "no newer copy on the server");
    }
    if (code != 200) {
        SetStore(s, sync::Phase::kFailed, local.version, server.version, other);
        return Outcome::kFailed;
    }
    const Taken t = hooks->take(s, dl, server.store_id, force, pin, said, said_size);
    remove(dl);
    if (t == Taken::kOk) {
        sync::Copy now;
        uint8_t seed[sync::kSeedIdSize];
        size_t size = 0;
        if (ReadLocal(s, &now, seed, &size)) {
            SaveSynced(s, now);
        }
        SetWaiting(s, sync::Copy(), false);
        SetStore(s, sync::Phase::kInSync, now.version);
        return Outcome::kTaken;
    }
    if (t == Taken::kNeedPin || t == Taken::kWrongPin) {
        SetWaiting(s, server, other);
        SetStore(s, sync::Phase::kNeedPin, local.version, server.version, other);
        if (t == Taken::kNeedPin) {
            snprintf(said, said_size, "type its PIN");
        }
        return Outcome::kWaits;
    }
    if (t == Taken::kLocked) {
        SetStore(s, sync::Phase::kNewer, local.version, server.version, other);
        return Outcome::kWaits;
    }
    if (t == Taken::kBad) {
        // Never taken: only a person can put ours over it.
        SetWaiting(s, server, other);
        SetStore(s, sync::Phase::kConflict, local.version, server.version, other);
        snprintf(said, said_size, "bad copy there");
        return Outcome::kWaits;
    }
    SetStore(s, sync::Phase::kFailed, local.version, server.version, other);
    return Outcome::kFailed;
}

// One store after heads: what Decide says, carried out.
Outcome SyncOne(const char *host, sync::Store s, const sync::Copy &local,
                const uint8_t seed_id[sync::kSeedIdSize], size_t size, const sync::Copy &server,
                bool open, Body &answer, char *said, size_t said_size) {
    sync::Copy was;
    {
        Locked l;
        was = synced[Index(s)];
    }
    const bool empty = open && hooks->empty(s);
    sync::Step step = sync::Decide(local, was, server, open, empty);
    const sync::Candidate &cand = looked[0];
    if (step == sync::Step::kLook) {
        char url[kSyncUrlSize];
        Call call = {url};
        call.answer = &answer;
        const int code = sync::StoresUrl(host, seed_id, url, sizeof url)
                             ? SignedCall(call, host, answer, said, said_size)
                             : -3;
        const int found = code == 200 ? sync::ParseStores(answer.data, answer.length,
                                                          sync::KindOf(s), local.store_id,
                                                          looked, sync::kMaxChoices)
                                      : -1;
        if (found < 0) {
            if (code == 200) {
                snprintf(said, said_size, "stores: not the server's answer");
            } else if (code > 0 || code == -1) {
                SayFailed(code, answer, said, said_size);
            }
            SetStore(s, sync::Phase::kFailed, local.version);
            return Outcome::kFailed;
        }
        if (found > 0 && !open) {
            // Ours may be empty: known once the PIN is in.
            SetStore(s, sync::Phase::kNewer, local.version, cand.copy.version, true, cand.at);
            return Outcome::kWaits;
        }
        step = sync::AfterLook(empty, static_cast<size_t>(found),
                               sync::SameWrite(local, server));
        if (step == sync::Step::kConflict) {
            const size_t n = static_cast<size_t>(found);
            SetWaiting(s, cand.copy, true, looked, n < sync::kMaxChoices ? n : sync::kMaxChoices);
            SetStore(s, sync::Phase::kConflict, local.version, cand.copy.version, true, cand.at);
            return Outcome::kWaits;
        }
    }
    switch (step) {
        case sync::Step::kNothing:
        case sync::Step::kLook:
            SetStore(s, sync::Phase::kInSync, local.version);
            return Outcome::kInSync;
        case sync::Step::kRecord:
            SaveSynced(s, local);
            SetStore(s, sync::Phase::kInSync, local.version);
            return Outcome::kInSync;
        case sync::Step::kUpload:
            return Send(host, s, local, size, answer, said, said_size);
        case sync::Step::kDownload:
            if (!open) {
                SetStore(s, sync::Phase::kNewer, local.version, server.version);
                return Outcome::kWaits;
            }
            return Take(host, s, local, seed_id, server, false, local.version, false, nullptr,
                        answer, said, said_size);
        case sync::Step::kAdopt:
            return Take(host, s, local, seed_id, cand.copy, true, 0, true, nullptr, answer, said,
                        said_size);
        case sync::Step::kConflict:
            SetWaiting(s, server, false);
            SetStore(s, sync::Phase::kConflict, local.version, server.version);
            return Outcome::kWaits;
    }
    return Outcome::kFailed;
}

void SyncSaid(const char *text, bool done) {
    Locked l;
    snprintf(status.sync_said, sizeof status.sync_said, "%s", text);
    if (done) {
        sync_done = sync_tried;
        status.synced_at = time(nullptr);
    }
}

// What a sync of several stores says.
void SayOutcomes(const Outcome *o, size_t n, const char *failed) {
    unsigned up = 0;
    unsigned down = 0;
    bool waits = false;
    bool bad = false;
    for (size_t i = 0; i < n; ++i) {
        up += o[i] == Outcome::kUploaded;
        down += o[i] == Outcome::kTaken;
        waits |= o[i] == Outcome::kWaits;
        bad |= o[i] == Outcome::kFailed;
    }
    char text[sizeof status.sync_said];
    if (bad) {
        snprintf(text, sizeof text, "%s", failed[0] != '\0' ? failed : "failed");
    } else if (waits) {
        snprintf(text, sizeof text, "%s", failed[0] != '\0' ? failed : "waits: see Sync");
    } else if (up + down == 0) {
        snprintf(text, sizeof text, "in sync");
    } else {
        snprintf(text, sizeof text, "sent %u, took %u", up, down);
    }
    SyncSaid(text, !bad);
}

// heads for our stores, then each one as Decide says.
void Sync(const Job &r, Body &answer) {
    sync::Copy local[sync::kStores];
    uint8_t seed[sync::kStores][sync::kSeedIdSize] = {};
    size_t size[sync::kStores] = {};
    sync::Entry entries[sync::kStores] = {};
    size_t count = 0;
    const bool open = hooks->open();
    for (const sync::Store s : sync::kAll) {
        const size_t i = Index(s);
        if (!ReadLocal(s, &local[i], seed[i], &size[i])) {
            SetStore(s, sync::Phase::kNone, 0);
            continue;
        }
        memcpy(entries[count].store_id, local[i].store_id, sync::kStoreIdSize);
        memcpy(entries[count].seed_id, seed[i], sync::kSeedIdSize);
        entries[count].kind = sync::KindOf(s);
        ++count;
    }
    if (count == 0) {
        SyncSaid("no stores here", true);
        return;
    }
    char said[sizeof status.sync_said] = "";
    char body[sync::kHeadsBodySize];
    char url[kSyncUrlSize];
    const size_t body_len = sync::HeadsBody(entries, count, body, sizeof body);
    int code = -3;
    if (body_len != 0 && Url(r.host, "/api/device/stores/heads", url, sizeof url)) {
        Call call = {url};
        call.type = "application/json";
        call.body = body;
        call.body_len = body_len;
        call.answer = &answer;
        code = SignedCall(call, r.host, answer, said, sizeof said);
    }
    // Every head first: the stores' requests reuse the answer's buffer.
    sync::Copy server[sync::kStores];
    bool named[sync::kStores] = {};
    for (const sync::Store s : sync::kAll) {
        const size_t i = Index(s);
        named[i] = code == 200 && local[i].has &&
                   sync::ParseHeads(answer.data, answer.length, local[i].store_id,
                                    sync::KindOf(s), &server[i]);
    }
    if (code != 200) {
        if (code > 0 || code == -1) {
            SayFailed(code, answer, said, sizeof said);
        }
        SyncSaid(said[0] != '\0' ? said : "heads failed", false);
        ESP_LOGI(TAG, "sync: heads %d", code);
        return;
    }
    Outcome o[sync::kStores];
    size_t n = 0;
    for (const sync::Store s : sync::kAll) {
        const size_t i = Index(s);
        if (!local[i].has) {
            continue;
        }
        if (!named[i]) {
            snprintf(said, sizeof said, "heads: not the server's answer");
            SetStore(s, sync::Phase::kFailed, local[i].version);
            o[n++] = Outcome::kFailed;
            continue;
        }
        char one[sizeof said - 12] = "";  // room for the store's name
        o[n] = SyncOne(r.host, s, local[i], seed[i], size[i], server[i], open, answer, one,
                       sizeof one);
        if (one[0] != '\0' && (o[n] == Outcome::kFailed || said[0] == '\0')) {
            snprintf(said, sizeof said, "%s: %s", sync::StoreName(s), one);
        }
        ESP_LOGI(TAG, "sync %s: v%u, outcome %d", sync::StoreName(s),
                 static_cast<unsigned>(local[i].version), static_cast<int>(o[n]));
        ++n;
    }
    SayOutcomes(o, n, said);
    // The account's stores under other seed phrases: shown only.
    if (r.by_hand || others_at < 0 || Uptime() - others_at >= kOthersSeconds) {
        const uint8_t *own = nullptr;
        for (size_t i = 0; i < sync::kStores && own == nullptr; ++i) {
            own = local[i].has ? seed[i] : nullptr;
        }
        char list_url[kSyncUrlSize];
        char why[40] = "";
        Call call = {list_url};
        call.answer = &answer;
        sync::Others others;
        if (own != nullptr && Url(r.host, "/api/device/stores", list_url, sizeof list_url) &&
            SignedCall(call, r.host, answer, why, sizeof why) == 200 &&
            sync::ParseOthers(answer.data, answer.length, own, local, &others)) {
            Locked l;
            others_at = Uptime();
            status.others_known = true;
            status.others = others;
        }
    }
    // What this sync did here is no change to sync again.
    Locked l;
    for (const sync::Store s : sync::kAll) {
        seen_version[Index(s)] = hooks->store_version(s);
    }
}

// A store that waits for a person: this device's copy, or the server's.
void Resolve(const Job &r, Body &answer) {
    const sync::Store s = r.store;
    const size_t i = Index(s);
    Waiting w;
    sync::Phase phase;
    {
        Locked l;
        w = waiting[i];
        phase = status.stores[i].phase;
        if (r.choice == Choice::kTakeServer && r.pick < status.stores[i].choices) {
            w = {choices[i][r.pick].copy, true};
        }
    }
    sync::Copy local;
    uint8_t seed[sync::kSeedIdSize];
    size_t size = 0;
    if (!sync::ByHand(phase) || !w.server.has || !ReadLocal(s, &local, seed, &size)) {
        SyncSaid("nothing waits", false);
        return;
    }
    char said[sizeof status.sync_said - 12] = "";  // room for the store's name
    Outcome o = Outcome::kFailed;
    if (r.choice == Choice::kKeepMine) {
        // Another store of the account: ours goes up beside it. Ours: above
        // the server's version, so it wins.
        const bool ok = w.other_store ||
                        (hooks->rewrite(s, sync::KeepVersion(w.server.version)) == ESP_OK &&
                         ReadLocal(s, &local, seed, &size));
        if (!ok) {
            snprintf(said, sizeof said, "cannot write our copy: unlock?");
        } else {
            o = Send(r.host, s, local, size, answer, said, sizeof said);
        }
    } else {
        // Taken by hand: also a version not above ours.
        const bool force = w.other_store || w.server.version <= local.version;
        o = Take(r.host, s, local, seed, w.server, w.other_store, force ? 0 : local.version,
                 force, r.pin[0] != '\0' ? r.pin : nullptr, answer, said, sizeof said);
    }
    char text[sizeof status.sync_said];
    snprintf(text, sizeof text, "%s: %s", sync::StoreName(s),
             said[0] != '\0'               ? said
             : o == Outcome::kUploaded     ? "this device's copy sent"
             : o == Outcome::kTaken        ? "the server's copy taken"
                                           : "waits");
    SyncSaid(text, o == Outcome::kUploaded || o == Outcome::kTaken);
    ESP_LOGI(TAG, "sync %s resolved: outcome %d", sync::StoreName(s), static_cast<int>(o));
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
    } else if (r->kind == Kind::kSync) {
        Sync(*r, answer);
    } else if (r->kind == Kind::kResolve) {
        Resolve(*r, answer);
    } else if (r->kind == Kind::kFirmware) {
        ListFirmware(*r);
    } else if (r->kind == Kind::kInstall) {
        Install(*r, answer);
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

esp_err_t Start(Kind kind, const char *host, const char *email, const Report *report,
                sync::Store store = sync::Store::kVault, Choice choice = Choice::kKeepMine,
                const char *pin = nullptr, bool by_hand = false,
                const firmware::Release *release = nullptr, uint8_t pick = 0) {
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
        const bool signed_in = kind != Kind::kCheck && kind != Kind::kRegister;
        if (signed_in && !(state.registered && state.client_id[0] != '\0')) {
            return ESP_ERR_NOT_FOUND;
        }
        status.busy = true;
        if (kind == Kind::kStatus) {
            status_tried = Uptime();
        }
        if (kind == Kind::kSync || kind == Kind::kResolve) {
            if (kind == Kind::kSync) {
                sync_tried = Uptime();
            }
            snprintf(status.sync_said, sizeof status.sync_said, "%s",
                     kind == Kind::kSync ? "syncing..." : "resolving...");
        } else if (kind == Kind::kFirmware || kind == Kind::kInstall) {
            snprintf(status.firmware_said, sizeof status.firmware_said, "%s",
                     kind == Kind::kFirmware ? "looking..." : "downloading...");
        } else if (kind != Kind::kStatus || !refused) {
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
        r->store = store;
        r->choice = choice;
        r->pick = pick;
        r->by_hand = by_hand;
        if (release != nullptr) {
            r->release = *release;
        }
        snprintf(r->pin, sizeof r->pin, "%s", pin != nullptr ? pin : "");
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

StaticTask_t scheduler_task;
// In PSRAM: the task never touches the flash (the cache is off meanwhile).
EXT_RAM_BSS_ATTR StackType_t scheduler_stack[kSchedulerStack / sizeof(StackType_t)];

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
            continue;  // one request at a time: the sync next look
        }
        // The sync: a change here shows on its store's line at once, and
        // starts one (so does the PIN coming in).
        uint32_t version[sync::kStores];
        for (const sync::Store s : sync::kAll) {
            version[Index(s)] = hooks->store_version(s);
        }
        const bool open = hooks->open();
        bool changed = open && !seen_open;
        bool by_hand = false;
        int64_t sync_tried_now;
        int64_t sync_done_now;
        {
            Locked l;
            for (size_t i = 0; i < sync::kStores; ++i) {
                StoreStatus &t = status.stores[i];
                changed |= version[i] != 0 && version[i] != seen_version[i];
                if (version[i] != 0 &&
                    (t.phase == sync::Phase::kInSync || t.phase == sync::Phase::kNone)) {
                    t.phase = synced[i].has && synced[i].version == version[i]
                                  ? sync::Phase::kInSync
                                  : sync::Phase::kChanged;
                    t.local_version = version[i];
                }
                by_hand |= sync::ByHand(t.phase);
            }
            sync_tried_now = sync_tried;
            sync_done_now = sync_done;
        }
        if (sync::SyncDue(ready, hooks->sync_minutes(), by_hand, changed, Uptime(),
                          sync_tried_now, sync_done_now)) {
            StartSync(false);
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
    LoadSynced();
    // A download cut short (a power loss) is no copy.
    for (const sync::Store s : sync::kAll) {
        const char *p = hooks->store_path(s);
        if (p != nullptr) {
            char dl[kPathSize + 32];
            snprintf(dl, sizeof dl, "%s.dl", p);
            remove(dl);
        }
    }
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
    sync::Phase phases[sync::kStores];
    for (size_t i = 0; i < sync::kStores; ++i) {
        phases[i] = s.stores[i].phase;
    }
    s.icon = sync::IconOf(hooks != nullptr, state.registered && state.client_id[0] != '\0',
                          phases);
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

esp_err_t StartSync(bool by_hand) {
    if (hooks == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    char host[kHostSize];
    hooks->host(host, sizeof host);
    uint32_t version[sync::kStores];
    for (const sync::Store s : sync::kAll) {
        version[static_cast<size_t>(s)] = hooks->store_version(s);
    }
    const bool open = hooks->open();
    const esp_err_t err = Start(Kind::kSync, host, nullptr, nullptr, sync::Store::kVault,
                                Choice::kKeepMine, nullptr, by_hand);
    if (err == ESP_OK) {
        Locked l;
        memcpy(seen_version, version, sizeof seen_version);
        seen_open = open;
    }
    return err;
}

esp_err_t StartResolve(sync::Store store, Choice choice, const char *pin, uint8_t pick) {
    if (hooks == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    {
        Locked l;
        const StoreStatus &t = status.stores[static_cast<size_t>(store)];
        if (!sync::ByHand(t.phase)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (pick != 0 && pick >= t.choices) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    char host[kHostSize];
    hooks->host(host, sizeof host);
    return Start(Kind::kResolve, host, nullptr, nullptr, store, choice, pin, false, nullptr, pick);
}

size_t Choices(sync::Store store, StoreChoice *out, size_t max) {
    if (lock == nullptr) {
        return 0;
    }
    Locked l;
    const size_t i = Index(store);
    size_t n = status.stores[i].choices;
    n = n < max ? n : max;
    for (size_t k = 0; k < n; ++k) {
        const sync::Candidate &c = choices[i][k];
        StoreChoice &o = out[k];
        o.version = c.copy.version;
        snprintf(o.at, sizeof o.at, "%s", c.at);
        snprintf(o.by, sizeof o.by, "%s", c.by);
        o.this_device = c.by[0] != '\0' && strcmp(c.by, state.client_id) == 0;
    }
    return n;
}

size_t Releases(firmware::Release *out, size_t max) {
    if (lock == nullptr) {
        return 0;
    }
    Locked l;
    const size_t n = release_count < max ? release_count : max;
    if (n > 0) {
        memcpy(out, releases, n * sizeof(firmware::Release));
    }
    return n;
}

esp_err_t StartFirmwareList() {
    if (hooks == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    char host[kHostSize];
    hooks->host(host, sizeof host);
    Report report = {};
    hooks->report(&report);  // the board
    return Start(Kind::kFirmware, host, nullptr, &report);
}

esp_err_t StartInstall(const char *id) {
    if (hooks == nullptr || ota::Refusal() != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    firmware::Release release = {};
    bool found = false;
    {
        Locked l;
        for (size_t i = 0; i < release_count && !found; ++i) {
            found = strcmp(releases[i].id, id) == 0;
            release = releases[i];
        }
    }
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    char host[kHostSize];
    hooks->host(host, sizeof host);
    return Start(Kind::kInstall, host, nullptr, nullptr, sync::Store::kVault,
                 Choice::kKeepMine, nullptr, false, &release);
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
