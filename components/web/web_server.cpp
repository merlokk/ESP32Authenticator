#include "web_server.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "ble.h"
#include "ble_fido.h"
#include "ble_kb.h"
#include "board_devices.h"
#include "config.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fido_ctap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "spiffs_fs.h"
#include "timesync.h"
#include "timezone.h"
#include "vault.h"
#include "web_admin_routes.h"
#include "web_auth.h"
#include "web_paths.h"
#include "web_settings.h"
#include "wifi_manager.h"

namespace web {
namespace {

constexpr const char *TAG = "web";

static_assert(config::kWebUserSize - 1 + 1 + config::kWebPasswordSize - 1 + 1 ==
                  kMaxCredentialSize,
              "the credential fields and the buffer that encodes them have drifted");

// --- embedded pages (www/, EMBED_FILES) ----------------------------------------

#define WWW_FILE(name, sym)                                                \
    extern const uint8_t name##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #sym "_end");

WWW_FILE(index_page, index_html)
WWW_FILE(wifi_page, wifi_html)
WWW_FILE(devstatus_page, devstatus_html)
WWW_FILE(hwinfo_page, hwinfo_html)
WWW_FILE(reboot_page, reboot_html)
WWW_FILE(not_found_page, 404_html)
WWW_FILE(unauthorised_page, 401_html)
WWW_FILE(app_script, app_js)
WWW_FILE(passwords_page, passwords_html)
WWW_FILE(fido_page, fido_html)

struct Page {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
};

const Page kPages[] = {
    {"index.html", index_page_start, index_page_end},
    {"wifi.html", wifi_page_start, wifi_page_end},
    {"devstatus.html", devstatus_page_start, devstatus_page_end},
    {"hwinfo.html", hwinfo_page_start, hwinfo_page_end},
    {"reboot.html", reboot_page_start, reboot_page_end},
    {"404.html", not_found_page_start, not_found_page_end},
    {"401.html", unauthorised_page_start, unauthorised_page_end},
    {"app.js", app_script_start, app_script_end},
    {"passwords.html", passwords_page_start, passwords_page_end},
    {"fido.html", fido_page_start, fido_page_end},
};

const Page *FindPage(const char *name) {
    for (const Page &page : kPages) {
        if (std::strcmp(page.name, name) == 0) {
            return &page;
        }
    }
    return nullptr;
}

// --- state ----------------------------------------------------------------------

httpd_handle_t g_server = nullptr;
// Start/Stop run on the manager's tick; the lock keeps the handle consistent
// if anything else ever calls them. Not recursive: *Locked halves below.
StaticSemaphore_t g_lock_storage;
SemaphoreHandle_t g_lock = nullptr;

class Held {
   public:
    Held() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Held() { xSemaphoreGive(g_lock); }
    Held(const Held &) = delete;
    Held &operator=(const Held &) = delete;
};

// httpd is one task serving one request at a time: these cannot be re-entered.
char g_json[1536];
char g_body[kMaxSettingsBody + 1];  // a POST body in, or a settings document out
constexpr size_t kMaxScan = 12;
wifi::ScanResult g_scan[kMaxScan];
constexpr size_t kMaxAuthHeader = kMaxEncodedSize + 24;
char g_auth[kMaxAuthHeader];

Status g_status;
Desired g_desired = Desired::kAuto;
DiagnosticsPrinter g_diagnostics = nullptr;
DiagnosticsPrinter g_hardware_info = nullptr;
void (*g_request_hook)() = nullptr;

// A failed start is retried every 5 s, not every tick.
constexpr uint32_t kRetryAfterMs = 5000;
uint32_t g_retry_at_ms = 0;
bool g_retry_pending = false;

void Count(uint32_t *field) { *field = *field + 1; }

void FormatIp(uint32_t ip, char *out, size_t size) {
    std::snprintf(out, size, "%u.%u.%u.%u", static_cast<unsigned>(ip & 0xFF),
                  static_cast<unsigned>((ip >> 8) & 0xFF), static_cast<unsigned>((ip >> 16) & 0xFF),
                  static_cast<unsigned>((ip >> 24) & 0xFF));
}

}  // namespace

// Shared with web_admin_routes.cpp (web_admin_routes.h).
namespace detail {

// JSON string escape (SSIDs are arbitrary bytes).
size_t Escape(const char *in, char *out, size_t size) {
    size_t n = 0;
    for (; *in != '\0' && n + 7 < size; ++in) {
        const unsigned char c = static_cast<unsigned char>(*in);
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = static_cast<char>(c);
        } else if (c < 0x20) {
            n += std::snprintf(out + n, size - n, "\\u%04x", c);
        } else {
            out[n++] = static_cast<char>(c);
        }
    }
    out[n] = '\0';
    return n;
}

}  // namespace detail

using namespace detail;

namespace {

esp_err_t SendPage(httpd_req_t *request, const Page &page) {
    httpd_resp_set_type(request, ContentType(page.name));
    const size_t size = static_cast<size_t>(page.end - page.start);
    g_status.bytes += static_cast<uint32_t>(size);
    return httpd_resp_send(request, reinterpret_cast<const char *>(page.start),
                           static_cast<ssize_t>(size));
}

// A refusal and a missing page are the same answer to the client.
esp_err_t NotFound(httpd_req_t *request) {
    Count(&g_status.not_found);
    httpd_resp_set_status(request, "404 Not Found");
    return SendPage(request, *FindPage(kNotFoundName));
}

}  // namespace

namespace detail {

esp_err_t Unauthorised(httpd_req_t *request) {
    Count(&g_status.unauthorised);
    httpd_resp_set_status(request, "401 Unauthorized");
    // What makes a browser show its login dialog.
    std::snprintf(g_json, sizeof g_json, "Basic realm=\"%s\", charset=\"UTF-8\"", kAuthRealm);
    httpd_resp_set_hdr(request, "WWW-Authenticate", g_json);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return SendPage(request, *FindPage(kUnauthorisedName));
}

// The live config, per request: a changed credential applies at once.
bool Allowed(httpd_req_t *request) {
    if (g_request_hook != nullptr) {
        g_request_hook();
    }
    const config::Web &web = config::Get().web;
    if (!AuthRequired(web.user, web.password)) {
        return true;
    }
    const size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
    if (length == 0 || length + 1 > sizeof g_auth) {
        return false;  // fail closed
    }
    if (httpd_req_get_hdr_value_str(request, "Authorization", g_auth, sizeof g_auth) != ESP_OK) {
        return false;
    }
    return Authorised(web.user, web.password, g_auth);
}

esp_err_t SendJson(httpd_req_t *request, const char *status, const char *json) {
    if (status != nullptr) {
        httpd_resp_set_status(request, status);
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, json);
}

esp_err_t Refuse(httpd_req_t *request, const char *status, const char *why) {
    std::snprintf(g_json, sizeof g_json, "{\"ok\":false,\"error\":\"%s\"}", why);
    return SendJson(request, status, g_json);
}

bool WritingAllowed() { return config::Get().web.write; }

void CountApi() { Count(&g_status.api); }

const char *Body() { return g_body; }

void WipeBody() { vault::Wipe(g_body, sizeof g_body); }

char *Scratch(size_t *size) {
    *size = sizeof g_json;
    return g_json;
}

}  // namespace detail

namespace {

// --- handlers -------------------------------------------------------------------

esp_err_t File(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    char name[kMaxNameLength + 1] = {};
    const Page *page = UriToName(request->uri, name, sizeof name) ? FindPage(name) : nullptr;
    if (page == nullptr) {
        return NotFound(request);
    }
    Count(&g_status.files);
    // Pages change only with the firmware.
    httpd_resp_set_hdr(request, "Cache-Control", "max-age=3600");
    return SendPage(request, *page);
}

// No passwords and no keys: state only.
esp_err_t StatusApi(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    Count(&g_status.api);
    const esp_app_desc_t *app = esp_app_get_description();
    const wifimgr::Snapshot wifi = wifimgr::Get();
    size_t total = 0;
    size_t used = 0;
    if (spiffs_fs::Mounted()) {
        spiffs_fs::Info(&total, &used);
    }
    uint32_t fido_requests = 0;
    uint32_t fido_errors = 0;
    ble_fido::Stats(&fido_requests, &fido_errors);
    // The store, unless a request holds it (then the page shows a dash).
    fido_ctap_info_t store = {};
    const bool have_store = fido_ctap_info(&store) == ESP_OK;
    // Fixed text from the store code (no quotes); "" while it runs.
    const char *fido_off = fido_ctap_stopped_reason();
    const bool ble_on = ble::IsOn();
    const bool ble_connected = ble_on && ble::ConnHandle() != BLE_HS_CONN_HANDLE_NONE;

    // UTC seconds (0: not set) and the zone's offset now; the page formats it.
    const time_t now = time(nullptr);
    const bool time_set = now >= 1704067200;  // 2024-01-01, rtc::Believable's floor
    const timesync::Status sync = timesync::Get();
    const char *sync_state = !sync.enabled          ? "off"
                             : sync.syncing         ? "syncing"
                             : sync.failures > 0    ? "failing"
                             : sync.ever_synced     ? "ok"
                             : !sync.internet       ? "waiting"
                                                    : "pending";
    char zone[2 * config::kZoneSize + 8];
    Escape(config::Get().time.zone, zone, sizeof zone);
    char ble_name[2 * config::kBleNameSize + 8];
    Escape(ble::Name(), ble_name, sizeof ble_name);

    char ssid[2 * wifi::kSsidSize + 8];
    char ap_ssid[2 * wifi::kSsidSize + 8];
    char ip[16];
    char ap_ip[16];
    Escape(wifi.radio.ssid, ssid, sizeof ssid);
    Escape(wifi.radio.ap_ssid, ap_ssid, sizeof ap_ssid);
    FormatIp(wifi.radio.ip, ip, sizeof ip);
    FormatIp(wifi.radio.ap_ip, ap_ip, sizeof ap_ip);
    // The station MAC: what a router's DHCP table and a MAC filter show.
    uint8_t mac_bytes[6] = {};
    esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
    char mac[18];
    std::snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", mac_bytes[0], mac_bytes[1],
                  mac_bytes[2], mac_bytes[3], mac_bytes[4], mac_bytes[5]);

    std::snprintf(
        g_json, sizeof g_json,
        "{\"firmware\":\"%s\",\"idf\":\"%s\",\"uptime_s\":%" PRIu32 ",\"heap_free\":%u"
        ",\"heap_low\":%u,\"psram_free\":%u,\"spiffs_used\":%u,\"spiffs_total\":%u"
        ",\"wifi\":\"%s\",\"wifi_mode\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"ip_static\":%s,\"internet\":\"%s\",\"mac\":\"%s\""
        ",\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"ap_clients\":%u"
        ",\"ble\":\"%s\",\"ble_secure\":%s,\"kb_ready\":%s,\"fido_ready\":%s"
        ",\"fido_requests\":%" PRIu32 ",\"fido_errors\":%" PRIu32
        ",\"fido_off\":\"%s\",\"fido_store\":%s"
        ",\"fido_creds\":%u,\"fido_resident\":%u,\"fido_max\":%u"
        ",\"fido_used\":%u,\"fido_size\":%u,\"fido_pin\":%s,\"fido_pin_retries\":%u"
        ",\"fido_counter\":%" PRIu32 ",\"fido_version\":%" PRIu32 ",\"fido_modified\":%lld"
        ",\"time_utc\":%lld,\"time_offset\":%d,\"time_zone\":\"%s\",\"time_sync\":\"%s\""
        ",\"time_sync_ago_s\":%ld,\"ble_name\":\"%s\",\"wallpaper\":\"%s\"}",
        app->version, app->idf_ver, static_cast<uint32_t>(esp_timer_get_time() / 1000000),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(used), static_cast<unsigned>(total), wifimgr::Name(wifi.state),
        wifimgr::Name(wifi.desired), ssid, static_cast<int>(wifi.radio.rssi), ip,
        wifi.radio.ip_is_static ? "true" : "false", wifimgr::Name(wifi.internet), mac, ap_ssid, ap_ip,
        static_cast<unsigned>(wifi.radio.clients),
        !ble_on ? "off" : ble_connected ? "connected" : "advertising",
        ble_connected && ble::LinkSecure() ? "true" : "false",
        ble_kb::Ready() ? "true" : "false", ble_fido::Ready() ? "true" : "false", fido_requests,
        fido_errors, fido_off != nullptr ? fido_off : "", have_store ? "true" : "false",
        static_cast<unsigned>(store.credentials),
        static_cast<unsigned>(store.discoverable), static_cast<unsigned>(store.max_credentials),
        static_cast<unsigned>(store.store_used), static_cast<unsigned>(store.store_size),
        store.pin_set ? "true" : "false", static_cast<unsigned>(store.pin_retries),
        store.sign_count, store.file_version, static_cast<long long>(store.modified_utc),
        time_set ? static_cast<long long>(now) : 0LL,
        time_set ? tz::OffsetSeconds(now) : 0, zone, sync_state,
        sync.since_last_ms == timesync::kNever ? -1L : static_cast<long>(sync.since_last_ms / 1000),
        ble_name, config::Name(config::Get().display.wallpaper));
    return SendJson(request, nullptr, g_json);
}

// --- /api/devstatus: the console's dump through a stdout swap -------------------

// picolibc's stdout is one global, so every task's printf lands here while the
// dump runs: only the owner task's writes become chunks, the rest go to the
// console (and never to a socket this task does not own).
struct Sink {
    httpd_req_t *request;
    TaskHandle_t owner;
    FILE *console;
    bool broken;
};

Sink g_sink;

int SinkWrite(void *cookie, const char *data, int size) {
    Sink *sink = static_cast<Sink *>(cookie);
    if (size <= 0) {
        return 0;
    }
    if (xTaskGetCurrentTaskHandle() != sink->owner) {
        if (sink->console != nullptr) {
            std::fwrite(data, 1, static_cast<size_t>(size), sink->console);
        }
        return size;
    }
    if (!sink->broken &&
        httpd_resp_send_chunk(sink->request, data, static_cast<size_t>(size)) != ESP_OK) {
        sink->broken = true;  // the browser went away; swallow the rest
    }
    return size;
}

// A console readout as text/plain, streamed as it is printed.
esp_err_t Dump(httpd_req_t *request, DiagnosticsPrinter printer) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    Count(&g_status.api);
    if (printer == nullptr) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain");
        return httpd_resp_sendstr(request, "no diagnostics wired up\n");
    }
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    g_sink = {request, xTaskGetCurrentTaskHandle(), stdout, false};
    FILE *sink = funopen(&g_sink, nullptr, SinkWrite, nullptr, nullptr);
    if (sink == nullptr) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        return httpd_resp_sendstr(request, "no stream\n");
    }
    // Unbuffered: a buffer would be shared with foreign printf calls.
    setvbuf(sink, nullptr, _IONBF, 0);
    FILE *saved = stdout;
    stdout = sink;
    printer();
    stdout = saved;
    std::fclose(sink);
    if (g_sink.broken) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t DevStatusApi(httpd_req_t *request) { return Dump(request, g_diagnostics); }

esp_err_t HardwareInfoApi(httpd_req_t *request) { return Dump(request, g_hardware_info); }

// --- settings ---------------------------------------------------------------------

// The document POST /api/settings takes, plus `writable` and the live `state`.
// No password leaves the device: `secured` says whether one is set.
esp_err_t SettingsGet(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    Count(&g_status.api);
    const config::Wifi &w = config::Get().wifi;
    const wifimgr::Snapshot now = wifimgr::Get();
    char ssid[2 * wifi::kSsidSize + 8];
    char ap_ssid[2 * wifi::kSsidSize + 8];
    char ip[16];
    Escape(now.radio.ssid, ssid, sizeof ssid);
    Escape(w.ap_ssid, ap_ssid, sizeof ap_ssid);
    FormatIp(now.radio.ip, ip, sizeof ip);

    constexpr int kSize = static_cast<int>(sizeof g_body);
    int written = std::snprintf(
        g_body, sizeof g_body,
        "{\"writable\":%s,\"max\":%u"
        ",\"state\":{\"wifi\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"failure\":\"%s\""
        ",\"ap_up\":%s,\"ap_clients\":%u}"
        ",\"wifi\":{\"mode\":\"%s\",\"ap\":{\"ssid\":\"%s\",\"secured\":%s,\"channel\":%u}"
        ",\"networks\":[",
        WritingAllowed() ? "true" : "false", static_cast<unsigned>(config::kMaxNetworks),
        wifimgr::Name(now.state), ssid, static_cast<int>(now.radio.rssi), ip,
        wifimgr::Name(now.failure),
        now.radio.mode == wifi::Mode::kAp || now.radio.mode == wifi::Mode::kApSta ? "true"
                                                                                  : "false",
        static_cast<unsigned>(now.radio.clients), w.active ? config::Name(w.mode) : "off",
        ap_ssid, w.ap_password[0] != '\0' ? "true" : "false",
        static_cast<unsigned>(w.ap_channel));
    for (uint8_t i = 0; i < w.network_count && written > 0 && written < kSize; ++i) {
        Escape(w.networks[i].ssid, ssid, sizeof ssid);
        // Addresses were validated as dotted quads: nothing to escape.
        const config::StaticIp &ip = w.networks[i].ip;
        written += std::snprintf(
            g_body + written, sizeof g_body - static_cast<size_t>(written),
            "%s{\"ssid\":\"%s\",\"secured\":%s,\"failed\":%s,\"ip\":{\"static\":%s"
            ",\"address\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"dns1\":\"%s\""
            ",\"dns2\":\"%s\"}}",
            i == 0 ? "" : ",", ssid, w.networks[i].password[0] != '\0' ? "true" : "false",
            now.auth_failed[i] ? "true" : "false", ip.enabled ? "true" : "false", ip.address,
            ip.netmask, ip.gateway, ip.dns1, ip.dns2);
    }
    if (written > 0 && written < kSize - 4 - 2 * static_cast<int>(config::kBleNameSize) - 24) {
        char ble_name[2 * config::kBleNameSize + 8];
        Escape(config::Get().ble.name, ble_name, sizeof ble_name);
        std::snprintf(g_body + written, sizeof g_body - static_cast<size_t>(written),
                      "]},\"ble\":{\"name\":\"%s\"}}", ble_name);
    }
    return SendJson(request, nullptr, g_body);
}

// Blocks a second or two: fine on this task, somebody asked.
esp_err_t WifiScanApi(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    Count(&g_status.api);
    size_t found = 0;
    if (wifimgr::Scan(g_scan, kMaxScan, &found) != ESP_OK) {
        return Refuse(request, "503 Service Unavailable", "the radio would not scan");
    }
    constexpr int kSize = static_cast<int>(sizeof g_body);
    int written = std::snprintf(g_body, sizeof g_body, "{\"networks\":[");
    size_t listed = 0;
    char ssid[2 * wifi::kSsidSize + 8];
    for (size_t i = 0; i < found && written > 0 && written < kSize; ++i) {
        if (g_scan[i].ssid[0] == '\0') {
            continue;  // hidden: nothing to pick
        }
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j) {
            seen = std::strcmp(g_scan[j].ssid, g_scan[i].ssid) == 0;
        }
        if (seen) {
            continue;  // strongest first, so the first one is kept
        }
        Escape(g_scan[i].ssid, ssid, sizeof ssid);
        written += std::snprintf(g_body + written, sizeof g_body - static_cast<size_t>(written),
                                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secured\":%s,\"channel\":%u}",
                                 listed == 0 ? "" : ",", ssid, static_cast<int>(g_scan[i].rssi),
                                 g_scan[i].secured ? "true" : "false",
                                 static_cast<unsigned>(g_scan[i].channel));
        ++listed;
    }
    if (written > 0 && written < kSize - 3) {
        std::snprintf(g_body + written, sizeof g_body - static_cast<size_t>(written), "]}");
    }
    return SendJson(request, nullptr, g_body);
}

}  // namespace

namespace detail {

// A body larger than the buffer is refused, not truncated.
bool ReadBody(httpd_req_t *request, size_t *length) {
    *length = 0;
    if (request->content_len > kMaxSettingsBody) {
        return false;
    }
    size_t taken = 0;
    while (taken < request->content_len) {
        const int got = httpd_req_recv(request, g_body + taken, request->content_len - taken);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            return false;
        }
        taken += static_cast<size_t>(got);
    }
    g_body[taken] = '\0';
    *length = taken;
    return true;
}

}  // namespace detail

namespace {

// Memory only; the radio is not re-applied here (that would drop the link
// this page came over): "retry" is its own action.
esp_err_t SettingsPost(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    Count(&g_status.api);
    if (!WritingAllowed()) {
        return Refuse(request, "403 Forbidden", "read-only: web.write is false");
    }
    size_t length = 0;
    if (!ReadBody(request, &length)) {
        return Refuse(request, "400 Bad Request", "body missing or too large");
    }
    const WriteOutcome outcome = ApplySettings(g_body, length, &config::Get());
    if (outcome.result != WriteResult::kOk) {
        ESP_LOGW(TAG, "settings refused: %s (%s)", WriteResultText(outcome.result),
                 outcome.detail);
        char detail[2 * sizeof outcome.detail + 8];
        Escape(outcome.detail, detail, sizeof detail);
        std::snprintf(g_json, sizeof g_json, "{\"ok\":false,\"error\":\"%s\",\"detail\":\"%s\"}",
                      WriteResultText(outcome.result), detail);
        return SendJson(request, "400 Bad Request", g_json);
    }
    if (outcome.ble_changed) {
        ble::SetName(config::Get().ble.name);
    }
    ESP_LOGI(TAG, "settings written over http (memory only until saved)");
    return SendJson(request, nullptr, "{\"ok\":true,\"saved\":false}");
}

esp_err_t ActionApi(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    Count(&g_status.api);
    if (!WritingAllowed()) {
        return Refuse(request, "403 Forbidden", "read-only: web.write is false");
    }
    const Action action = ActionFromUri(request->uri);
    esp_err_t err = ESP_OK;
    switch (action) {
        case Action::kSave:
            err = config::Save();
            break;
        case Action::kReload:
            // May take this page with it: the reloaded networks are applied.
            err = config::Reload();
            if (err == ESP_OK) {
                wifimgr::Apply();
                tz::ApplyZone(config::Get().time.zone);
                timesync::Apply();
                boards::SetLedBrightness(config::Get().led.percent,
                                         config::Get().led.idle_percent);
                Apply();
            }
            break;
        case Action::kWifiRetry:
            wifimgr::Apply();
            break;
        case Action::kNone:
            return Refuse(request, "400 Bad Request", "no such action");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", ActionName(action), esp_err_to_name(err));
        return Refuse(request, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "%s, asked for over http", ActionName(action));
    std::snprintf(g_json, sizeof g_json, "{\"ok\":true,\"did\":\"%s\"}", ActionName(action));
    return SendJson(request, nullptr, g_json);
}

// POST only, and only with the word: no link or prefetch can restart the device.
esp_err_t RebootApi(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    Count(&g_status.api);
    if (!ConfirmsReboot(request->uri)) {
        return Refuse(request, "400 Bad Request", "not confirmed");
    }
    ESP_LOGW(TAG, "reboot asked for over http");
    const esp_err_t err = SendJson(request, nullptr, "{\"ok\":true,\"rebooting\":true}");
    // Let the answer leave before the chip does.
    vTaskDelay(pdMS_TO_TICKS(500));
    if (ble::IsOn()) {
        ble::Off();  // drop the BLE link cleanly, as the console's reboot does
    }
    esp_restart();
    return err;
}

// --- lifetime -------------------------------------------------------------------

esp_err_t StartLocked() {
    if (g_server != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // httpd_start without lwIP asserts in tcpip_send_msg_wait_sem.
    if (!wifimgr::Get().stack_up) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPort;
    config.task_priority = 2;
    config.stack_size = 4096;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;  // a browser's idle socket must not block the next
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&g_server, &config);
    if (err != ESP_OK) {
        g_server = nullptr;
        ESP_LOGE(TAG, "not started: %s", esp_err_to_name(err));
        return err;
    }
    // The API first: "/*" matches everything and the first match wins. The
    // file handler is GET-only, so every other route is read-only by design.
    const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = StatusApi, .user_ctx = nullptr},
        {.uri = "/api/devstatus", .method = HTTP_GET, .handler = DevStatusApi, .user_ctx = nullptr},
        {.uri = "/api/hwinfo", .method = HTTP_GET, .handler = HardwareInfoApi, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = SettingsGet, .user_ctx = nullptr},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = WifiScanApi, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = SettingsPost, .user_ctx = nullptr},
        {.uri = "/api/action", .method = HTTP_POST, .handler = ActionApi, .user_ctx = nullptr},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = RebootApi, .user_ctx = nullptr},
    };
    for (const httpd_uri_t &route : routes) {
        err = httpd_register_uri_handler(g_server, &route);
        if (err != ESP_OK) {
            httpd_stop(g_server);
            g_server = nullptr;
            ESP_LOGE(TAG, "handler %s: %s", route.uri, esp_err_to_name(err));
            return err;
        }
    }
    // Then the password and FIDO2 pages' API, then the files ("/*" last).
    err = RegisterAdminRoutes(g_server);
    if (err == ESP_OK) {
        const httpd_uri_t files = {
            .uri = "/*", .method = HTTP_GET, .handler = File, .user_ctx = nullptr};
        err = httpd_register_uri_handler(g_server, &files);
    }
    if (err != ESP_OK) {
        httpd_stop(g_server);
        g_server = nullptr;
        ESP_LOGE(TAG, "handlers: %s", esp_err_to_name(err));
        return err;
    }
    g_status.cost =
        static_cast<uint32_t>(before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "serving on port %u", static_cast<unsigned>(kPort));
    return ESP_OK;
}

esp_err_t StopLocked() {
    if (g_server == nullptr) {
        return ESP_OK;
    }
    const esp_err_t err = httpd_stop(g_server);
    if (err != ESP_OK) {
        // httpd_stop returns before doing anything when it cannot signal its
        // task: the server still runs, keep the handle and try again.
        ESP_LOGE(TAG, "would not stop (%s), will try again", esp_err_to_name(err));
        return err;
    }
    g_server = nullptr;
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

// On the Wi-Fi manager's tick, outside its lock.
void Maintain() {
    const wifimgr::Snapshot wifi = wifimgr::Get();
    const bool ap = wifi.radio.mode == wifi::Mode::kAp || wifi.radio.mode == wifi::Mode::kApSta;
    // The file is what the console just edited and the snapshot what the
    // policy does: either saying off lets go before the radio goes.
    const bool network_wanted = wifimgr::DesiredFromConfig() != wifimgr::Desired::kOff &&
                                wifi.desired != wifimgr::Desired::kOff;
    const Held held;
    const Reconcile next = Next(g_desired, network_wanted, wifi.stack_up, ap, g_server != nullptr);
    if (next == Reconcile::kStop) {
        StopLocked();
        return;
    }
    if (next != Reconcile::kStart) {
        return;
    }
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (g_retry_pending && now - g_retry_at_ms < kRetryAfterMs) {
        return;
    }
    g_retry_pending = StartLocked() != ESP_OK;
    g_retry_at_ms = now;
}

}  // namespace

esp_err_t Init() {
    g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
    if (g_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    g_desired = DesiredFromConfig();
    wifimgr::OnTick([](void *) { Maintain(); }, nullptr);
    return ESP_OK;
}

void SetDesired(Desired desired) {
    g_desired = desired;
    g_retry_pending = false;  // asked now: try now
}

Desired DesiredFromConfig() {
    switch (config::Get().web.mode) {
        case config::WebMode::kOff: return Desired::kOff;
        case config::WebMode::kOn: return Desired::kOn;
        case config::WebMode::kAuto: break;
    }
    return Desired::kAuto;
}

void Apply() { SetDesired(DesiredFromConfig()); }

void SetDiagnostics(DiagnosticsPrinter printer) { g_diagnostics = printer; }

void SetHardwareInfo(DiagnosticsPrinter printer) { g_hardware_info = printer; }

void SetRequestHook(void (*hook)()) { g_request_hook = hook; }

bool Running() { return g_server != nullptr; }

Status Get() {
    Status out = g_status;
    out.desired = g_desired;
    out.running = g_server != nullptr;
    out.auth = AuthRequired(config::Get().web.user, config::Get().web.password);
    return out;
}

}  // namespace web
