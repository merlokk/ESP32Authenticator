#include "web_admin_routes.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "clock_math.h"
#include "codes.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fido_ctap.h"
#include "timezone.h"
#include "vault.h"
#include "web_admin.h"

namespace web {
namespace {

using namespace detail;

constexpr const char *TAG = "web";

// Static, not on the httpd stack; httpd serves one request at a time. Both
// hold secrets: wiped after every use (Wiped).
vault::Record g_record;
AdminRequest g_request;
// Escaped text for one record or credential.
char g_text[3][2 * vault::kSiteMax + 8];

struct Wiped {
    ~Wiped() {
        vault::Wipe(&g_record, sizeof g_record);
        vault::Wipe(&g_request, sizeof g_request);
    }
};

bool TimeKnown(time_t now) { return rtc::Believable(static_cast<int64_t>(now)); }

esp_err_t Chunk(httpd_req_t *request, const char *text) {
    return httpd_resp_send_chunk(request, text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t StartJson(httpd_req_t *request) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return ESP_OK;
}

// {"ok":false,"error":..., "detail":...}; texts are the firmware's own.
esp_err_t Fail(httpd_req_t *request, const char *status, const char *error, const char *detail) {
    size_t size = 0;
    char *json = Scratch(&size);
    Escape(error, g_text[0], sizeof g_text[0]);
    Escape(detail, g_text[1], sizeof g_text[1]);
    std::snprintf(json, size, "{\"ok\":false,\"error\":\"%s\",\"detail\":\"%s\"}", g_text[0],
                  g_text[1]);
    return SendJson(request, status, json);
}

esp_err_t VaultFailed(httpd_req_t *request, vault::Error e) {
    const vault::Info info = vault::GetInfo();
    char detail[64] = "";
    const char *status = "400 Bad Request";
    switch (e) {
        case vault::Error::kLocked:
            status = "409 Conflict";
            std::snprintf(detail, sizeof detail, "%s",
                          info.state == vault::State::kRefused && info.why != nullptr
                              ? info.why
                              : "enter the PIN");
            break;
        case vault::Error::kNotFound:
            status = "404 Not Found";
            break;
        case vault::Error::kWrongPin:
            status = "403 Forbidden";
            std::snprintf(detail, sizeof detail, "%u of %u tries left",
                          static_cast<unsigned>(vault::kMaxPinTries - info.pin_failures),
                          static_cast<unsigned>(vault::kMaxPinTries));
            break;
        case vault::Error::kPinLocked:
            status = "429 Too Many Requests";
            std::snprintf(detail, sizeof detail, "%u s left",
                          static_cast<unsigned>(info.lock_left_s));
            break;
        case vault::Error::kStorage:
            status = "500 Internal Server Error";
            break;
        case vault::Error::kDuplicate:
        case vault::Error::kFull:
            status = "409 Conflict";
            break;
        default:
            break;
    }
    return Fail(request, status, vault::Describe(e), detail);
}

// Basic auth, web.write, and a JSON content type (no cross-site form).
bool PostAllowed(httpd_req_t *request, esp_err_t *answer) {
    if (!Allowed(request)) {
        *answer = Unauthorised(request);
        return false;
    }
    CountApi();
    if (!WritingAllowed()) {
        *answer = Refuse(request, "403 Forbidden", "read-only: web.write is false");
        return false;
    }
    char type[48] = "";
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof type) != ESP_OK ||
        !IsJsonType(type)) {
        *answer = Refuse(request, "415 Unsupported Media Type", "send application/json");
        return false;
    }
    return true;
}

// The body into g_request; the raw body is wiped at once.
bool ReadRequest(httpd_req_t *request, esp_err_t *answer) {
    size_t length = 0;
    if (!ReadBody(request, &length)) {
        WipeBody();
        *answer = Refuse(request, "400 Bad Request", "body missing or too large");
        return false;
    }
    const BodyOutcome outcome = ParseAdminRequest(Body(), length, &g_request);
    WipeBody();
    if (outcome.result != BodyResult::kOk) {
        *answer = Fail(request, "400 Bad Request", BodyResultText(outcome.result), outcome.detail);
        return false;
    }
    return true;
}

// --- passwords ----------------------------------------------------------------------

esp_err_t VaultGet(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    CountApi();
    Wiped wipe;
    const vault::Info info = vault::GetInfo();
    const time_t now = time(nullptr);
    const bool time_known = TimeKnown(now);
    size_t size = 0;
    char *json = Scratch(&size);
    Escape(info.why != nullptr ? info.why : "", g_text[0], sizeof g_text[0]);
    std::snprintf(
        json, size,
        "{\"state\":\"%s\",\"why\":\"%s\",\"writable\":%s,\"count\":%u,\"capacity\":%u"
        ",\"version\":%" PRIu32 ",\"modified\":%lld,\"pin_failures\":%u,\"pin_tries\":%u"
        ",\"lock_left_s\":%" PRIu32 ",\"time_utc\":%lld,\"time_offset\":%d,\"records\":[",
        vault::StateName(info.state), g_text[0], WritingAllowed() ? "true" : "false",
        static_cast<unsigned>(info.count), static_cast<unsigned>(info.capacity), info.version,
        static_cast<long long>(info.modified_utc), static_cast<unsigned>(info.pin_failures),
        static_cast<unsigned>(vault::kMaxPinTries), info.lock_left_s,
        time_known ? static_cast<long long>(now) : 0LL, time_known ? tz::OffsetSeconds(now) : 0);
    StartJson(request);
    esp_err_t err = Chunk(request, json);
    for (size_t i = 0; err == ESP_OK && vault::Get(i, &g_record) == vault::Error::kOk; ++i) {
        Escape(g_record.site, g_text[0], sizeof g_text[0]);
        Escape(g_record.user, g_text[1], sizeof g_text[1]);
        std::snprintf(json, size,
                      "%s{\"id\":%" PRIu32 ",\"site\":\"%s\",\"user\":\"%s\",\"password\":%s"
                      ",\"otp\":%s,\"modified\":%lld}",
                      i == 0 ? "" : ",", g_record.id, g_text[0], g_text[1],
                      g_record.password[0] != '\0' ? "true" : "false",
                      g_record.otp[0] != '\0' ? "true" : "false",
                      static_cast<long long>(g_record.modified_utc));
        err = Chunk(request, json);
    }
    if (err == ESP_OK) {
        err = Chunk(request, "]}");
    }
    return err == ESP_OK ? httpd_resp_send_chunk(request, nullptr, 0) : ESP_FAIL;
}

esp_err_t TotpGet(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    CountApi();
    Wiped wipe;
    uint32_t id = 0;
    if (!IdFromQuery(request->uri, &id)) {
        return Refuse(request, "400 Bad Request", "id=<record id> missing");
    }
    const time_t now = time(nullptr);
    if (!TimeKnown(now)) {
        return Refuse(request, "409 Conflict", "the device has no time yet");
    }
    const long position = vault::Position(id);
    if (position < 0) {
        return VaultFailed(request, vault::GetInfo().state == vault::State::kOpen
                                        ? vault::Error::kNotFound
                                        : vault::Error::kLocked);
    }
    const vault::Error e = vault::Get(static_cast<size_t>(position), &g_record);
    if (e != vault::Error::kOk) {
        return VaultFailed(request, e);
    }
    if (g_record.id != id) {
        return VaultFailed(request, vault::Error::kNotFound);  // changed meanwhile
    }
    if (g_record.otp[0] == '\0') {
        return Refuse(request, "404 Not Found", "this record has no TOTP secret");
    }
    totp::Codes codes = {};
    const esp_err_t err = totp::Generate(g_record.otp, static_cast<int64_t>(now), &codes);
    if (err != ESP_OK) {
        return Refuse(request, "500 Internal Server Error", esp_err_to_name(err));
    }
    size_t size = 0;
    char *json = Scratch(&size);
    std::snprintf(json, size, "{\"code\":\"%06" PRIu32 "\",\"remaining\":%" PRIu32 ",\"step\":%u}",
                  codes.code[1], codes.windows.remaining, static_cast<unsigned>(totp::kStep));
    vault::Wipe(&codes, sizeof codes);
    const esp_err_t sent = SendJson(request, nullptr, json);
    vault::Wipe(json, size);
    return sent;
}

constexpr vault::Field kFields[] = {vault::Field::kSite, vault::Field::kUser,
                                    vault::Field::kPassword, vault::Field::kOtp};
constexpr const char *kFieldNames[] = {"site", "user", "password", "otp"};

bool AnyField() {
    for (bool has : g_request.has) {
        if (has) {
            return true;
        }
    }
    return false;
}

// Every field given is checked before any is written.
vault::Error SetFields(const char **failed_field) {
    for (size_t i = 0; i < 4; ++i) {
        if (!g_request.has[i]) {
            continue;
        }
        // Into the largest field of the scratch record (wiped with it).
        const vault::Error e = vault::CheckField(kFields[i], FieldValue(g_request, kFields[i]),
                                                 g_record.password);
        if (e != vault::Error::kOk) {
            *failed_field = kFieldNames[i];
            return e;
        }
    }
    for (size_t i = 0; i < 4; ++i) {
        if (g_request.has[i]) {
            const vault::Error e =
                vault::Set(g_request.id, kFields[i], FieldValue(g_request, kFields[i]));
            if (e != vault::Error::kOk) {
                *failed_field = kFieldNames[i];
                return e;
            }
        }
    }
    return vault::Error::kOk;
}

esp_err_t VaultPost(httpd_req_t *request) {
    esp_err_t answer = ESP_OK;
    if (!PostAllowed(request, &answer)) {
        return answer;
    }
    const VaultVerb verb = VaultVerbFromUri(request->uri);
    if (verb == VaultVerb::kNone) {
        return Refuse(request, "404 Not Found", "no such action");
    }
    Wiped wipe;
    if (!ReadRequest(request, &answer)) {
        return answer;
    }
    vault::Error e = vault::Error::kOk;
    const char *field = "";
    uint32_t id = g_request.id;
    switch (verb) {
        case VaultVerb::kAdd:
            if (!g_request.has[0]) {
                return Fail(request, "400 Bad Request", "missing", "site");
            }
            e = vault::Add(g_request.site, g_request.user, g_request.password,
                           g_request.otp, &id);
            break;
        case VaultVerb::kSet:
            if (!g_request.has_id) {
                return Fail(request, "400 Bad Request", "missing", "id");
            }
            if (!AnyField()) {
                return Fail(request, "400 Bad Request", "missing", "site, user, password or otp");
            }
            e = SetFields(&field);
            break;
        case VaultVerb::kDelete:
            if (!g_request.has_id) {
                return Fail(request, "400 Bad Request", "missing", "id");
            }
            e = vault::Remove(g_request.id);
            break;
        case VaultVerb::kUnlock:
            if (!g_request.has_pin) {
                return Fail(request, "400 Bad Request", "missing", "pin");
            }
            e = vault::Unlock(g_request.pin);
            break;
        case VaultVerb::kLock:
            vault::Lock();
            break;
        case VaultVerb::kNone:
            break;
    }
    // The verb and the outcome only; a wipe PIN reads as a wrong one here too.
    ESP_LOGI(TAG, "vault %s over http: %s", VaultVerbName(verb), vault::Describe(e));
    if (e != vault::Error::kOk) {
        // A field's own refusal names it; a record gone or a locked store do not.
        if (field[0] != '\0' && e != vault::Error::kNotFound && e != vault::Error::kLocked &&
            e != vault::Error::kStorage) {
            return Fail(request, e == vault::Error::kDuplicate ? "409 Conflict" : "400 Bad Request",
                        vault::Describe(e), field);
        }
        return VaultFailed(request, e);
    }
    size_t size = 0;
    char *json = Scratch(&size);
    std::snprintf(json, size, "{\"ok\":true,\"did\":\"%s\",\"id\":%" PRIu32 "}",
                  VaultVerbName(verb), id);
    return SendJson(request, nullptr, json);
}

// --- FIDO2 ------------------------------------------------------------------------

esp_err_t FidoFailed(httpd_req_t *request, esp_err_t err) {
    if (err == ESP_ERR_TIMEOUT) {
        return Refuse(request, "503 Service Unavailable",
                      "busy: a FIDO request is running (waiting for the button?)");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return Refuse(request, "404 Not Found", "no such credential");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        const char *why = fido_ctap_stopped_reason();
        return Fail(request, "409 Conflict", "FIDO2 is off",
                    why != nullptr ? why : "the core is not running");
    }
    return Refuse(request, "500 Internal Server Error", esp_err_to_name(err));
}

void Hex(const uint8_t *data, size_t size, char *out) {
    for (size_t i = 0; i < size; ++i) {
        std::snprintf(out + 2 * i, 3, "%02x", data[i]);
    }
}

void Mac(const uint8_t *mac, char *out, size_t size) {
    std::snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
}

esp_err_t FidoGet(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    CountApi();
    fido_ctap_info_t info = {};
    esp_err_t err = fido_ctap_info(&info);
    if (err != ESP_OK) {
        return FidoFailed(request, err);
    }
    const size_t capacity = info.max_credentials;
    auto *list = static_cast<fido_ctap_credential_t *>(heap_caps_malloc(
        capacity * sizeof(fido_ctap_credential_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (list == nullptr) {
        list = static_cast<fido_ctap_credential_t *>(
            std::malloc(capacity * sizeof(fido_ctap_credential_t)));
    }
    if (list == nullptr) {
        return Refuse(request, "500 Internal Server Error", "no memory");
    }
    size_t count = 0;
    err = fido_ctap_list(list, capacity, &count);
    if (err != ESP_OK) {
        std::free(list);
        return FidoFailed(request, err);
    }
    const time_t now = time(nullptr);
    size_t size = 0;
    char *json = Scratch(&size);
    std::snprintf(
        json, size,
        "{\"writable\":%s,\"count\":%u,\"max\":%u,\"resident\":%u,\"used\":%u,\"size\":%u"
        ",\"pin_set\":%s,\"pin_retries\":%u,\"counter\":%" PRIu32 ",\"version\":%" PRIu32
        ",\"modified\":%lld,\"time_offset\":%d,\"credentials\":[",
        WritingAllowed() ? "true" : "false", static_cast<unsigned>(count),
        static_cast<unsigned>(info.max_credentials), static_cast<unsigned>(info.discoverable),
        static_cast<unsigned>(info.store_used), static_cast<unsigned>(info.store_size),
        info.pin_set ? "true" : "false", static_cast<unsigned>(info.pin_retries), info.sign_count,
        info.file_version, static_cast<long long>(info.modified_utc),
        TimeKnown(now) ? tz::OffsetSeconds(now) : 0);
    StartJson(request);
    err = Chunk(request, json);
    for (size_t i = 0; i < count && err == ESP_OK; ++i) {
        const fido_ctap_credential_t &c = list[i];
        char id[2 * FIDO_CTAP_CRED_ID_SIZE + 1];
        Hex(c.id, sizeof c.id, id);
        char device[18] = "";
        if (c.origin.has_device) {
            Mac(c.origin.device, device, sizeof device);
        }
        char host[32] = "";
        if (c.origin.client.transport == FIDO_CTAP_TRANSPORT_BLE) {
            Mac(c.origin.client.addr, host, sizeof host);
            std::strncat(host, " (BLE)", sizeof host - std::strlen(host) - 1);
        } else if (c.origin.client.transport == FIDO_CTAP_TRANSPORT_USB) {
            std::snprintf(host, sizeof host, "USB");
        }
        Escape(c.rp_id, g_text[0], sizeof g_text[0]);
        Escape(c.user_name, g_text[1], sizeof g_text[1]);
        Escape(c.display_name, g_text[2], sizeof g_text[2]);
        std::snprintf(json, size,
                      "%s{\"id\":\"%s\",\"rp\":\"%s\",\"rp_cut\":%s,\"user\":\"%s\""
                      ",\"display\":\"%s\",\"resident\":%s,\"u2f\":%s,\"protect\":%u,\"alg\":%d,\"created\":%lld"
                      ",\"device\":\"%s\",\"host\":\"%s\"}",
                      i == 0 ? "" : ",", id, g_text[0], c.rp_id_truncated ? "true" : "false",
                      g_text[1], g_text[2], c.discoverable ? "true" : "false",
                      c.u2f ? "true" : "false", static_cast<unsigned>(c.cred_protect), static_cast<int>(c.alg),
                      static_cast<long long>(c.origin.created_utc), device, host);
        err = Chunk(request, json);
    }
    std::free(list);
    if (err == ESP_OK) {
        err = Chunk(request, "]}");
    }
    return err == ESP_OK ? httpd_resp_send_chunk(request, nullptr, 0) : ESP_FAIL;
}

esp_err_t FidoDelete(httpd_req_t *request) {
    esp_err_t answer = ESP_OK;
    if (!PostAllowed(request, &answer)) {
        return answer;
    }
    Wiped wipe;
    if (!ReadRequest(request, &answer)) {
        return answer;
    }
    if (!g_request.has_credential) {
        return Fail(request, "400 Bad Request", "missing", "credential");
    }
    const esp_err_t err = fido_ctap_delete(g_request.credential);
    ESP_LOGI(TAG, "fido2 delete over http: %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        return FidoFailed(request, err);
    }
    return SendJson(request, nullptr, "{\"ok\":true,\"did\":\"delete\"}");
}

}  // namespace

esp_err_t RegisterAdminRoutes(httpd_handle_t server) {
    const httpd_uri_t routes[] = {
        {.uri = "/api/vault", .method = HTTP_GET, .handler = VaultGet, .user_ctx = nullptr},
        {.uri = "/api/vault/totp", .method = HTTP_GET, .handler = TotpGet, .user_ctx = nullptr},
        {.uri = "/api/vault/*", .method = HTTP_POST, .handler = VaultPost, .user_ctx = nullptr},
        {.uri = "/api/fido", .method = HTTP_GET, .handler = FidoGet, .user_ctx = nullptr},
        {.uri = "/api/fido/delete", .method = HTTP_POST, .handler = FidoDelete,
         .user_ctx = nullptr},
    };
    for (const httpd_uri_t &route : routes) {
        const esp_err_t err = httpd_register_uri_handler(server, &route);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

}  // namespace web
