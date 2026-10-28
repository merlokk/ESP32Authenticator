// The Firmware page's API (web_admin_routes.h): the OTA slots, a .bin
// uploaded from the browser or /sdcard/firmware.bin (ota_update.h).

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_update.h"
#include "vault.h"
#include "web_admin.h"
#include "web_admin_routes.h"
#include "web_paths.h"

namespace web {
namespace {

using namespace detail;

constexpr const char *TAG = "web";
constexpr size_t kChunk = 4096;
constexpr int kMaxStalls = 6;  // receive timeouts in a row before the upload is dropped

// Static, not on the httpd stack; one request at a time.
EXT_RAM_BSS_ATTR ota::Status g_ota;
ota::SdFile g_sd;
EXT_RAM_BSS_ATTR char g_esc[6][72];

const char *Esc(int slot, const char *text) {
    Escape(text, g_esc[slot], sizeof g_esc[slot]);
    return g_esc[slot];
}

// Why this site may not update now; nullptr when it may.
const char *Gate() {
    if (!WritingAllowed()) {
        return "read-only: web.write is false";
    }
    if (vault::GetState() == vault::State::kLocked) {
        return "locked: unlock with the PIN first";
    }
    return ota::Refusal();
}

esp_err_t FirmwareGet(httpd_req_t *request) {
    if (!Allowed(request)) {
        return Unauthorised(request);
    }
    CountApi();
    ota::GetStatus(&g_ota);
    ota::ReadSdFile(&g_sd);
    const char *gate = Gate();
    size_t size = 0;
    char *json = Scratch(&size);
    const ota::AppInfo &sd = g_sd.info;
    const bool sd_ok = g_sd.present && g_sd.verdict == ota::Verdict::kMore;
    std::snprintf(
        json, size,
        "{\"running\":{\"slot\":\"%s\",\"version\":\"%s\",\"date\":\"%s %s\",\"idf\":\"%s\","
        "\"state\":\"%s\"},\"trial\":%s,\"confirm_in\":%" PRIu32 ","
        "\"next\":{\"slot\":\"%s\",\"size\":%u},\"rolled_back\":\"%s\","
        "\"job\":{\"state\":\"%s\",\"source\":\"%s\",\"written\":%u,\"total\":%u,"
        "\"error\":\"%s\",\"version\":\"%s\"},"
        "\"sd\":{\"mounted\":%s,\"present\":%s,\"size\":%u,\"ok\":%s,\"check\":\"%s\","
        "\"version\":\"%s\",\"date\":\"%s %s\",\"same\":%s},"
        "\"refusal\":\"%s\"}",
        g_ota.running_slot, Esc(0, g_ota.running_version), g_ota.running_date, g_ota.running_time,
        g_ota.running_idf, g_ota.running_state, g_ota.trial ? "true" : "false", g_ota.confirm_in_s,
        g_ota.next_slot, static_cast<unsigned>(g_ota.next_size), Esc(1, g_ota.rolled_back),
        ota::JobName(g_ota.job), ota::SourceName(g_ota.source),
        static_cast<unsigned>(g_ota.written), static_cast<unsigned>(g_ota.total), g_ota.error,
        Esc(2, g_ota.job_version), g_sd.mounted ? "true" : "false",
        g_sd.present ? "true" : "false", static_cast<unsigned>(g_sd.size),
        sd_ok ? "true" : "false", g_sd.present ? (sd_ok ? "ok" : ota::VerdictName(g_sd.verdict)) : "",
        Esc(3, sd.version), Esc(4, sd.date), Esc(5, sd.time), g_sd.same_as_running ? "true" : "false",
        gate != nullptr ? gate : "");
    return SendJson(request, nullptr, json);
}

// The common refusals of both POSTs; true when the request may go on.
bool MayUpdate(httpd_req_t *request, esp_err_t *answer) {
    if (!Allowed(request)) {
        *answer = Unauthorised(request);
        return false;
    }
    CountApi();
    const char *gate = Gate();
    if (gate != nullptr) {
        *answer = Refuse(request, WritingAllowed() ? "409 Conflict" : "403 Forbidden", gate);
        return false;
    }
    if (!Confirms(request->uri, "update")) {
        *answer = Refuse(request, "400 Bad Request", "not confirmed");
        return false;
    }
    return true;
}

// The rest of an unread body would be taken for the next request.
esp_err_t RefuseAndClose(httpd_req_t *request, const char *status, const char *why) {
    httpd_resp_set_hdr(request, "Connection", "close");
    const esp_err_t err = Refuse(request, status, why);
    httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
    return err;
}

esp_err_t UploadPost(httpd_req_t *request) {
    esp_err_t answer = ESP_OK;
    if (!MayUpdate(request, &answer)) {
        return answer;
    }
    char type[48] = "";
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof type) != ESP_OK ||
        !IsType(type, "application/octet-stream")) {
        return RefuseAndClose(request, "415 Unsupported Media Type",
                              "send application/octet-stream");
    }
    if (request->content_len == 0) {
        return Refuse(request, "400 Bad Request", "no firmware in the body");
    }
    if (ota::Begin(ota::Source::kWeb, request->content_len) != ESP_OK) {
        ota::GetStatus(&g_ota);
        return RefuseAndClose(request, "409 Conflict",
                              g_ota.error[0] != '\0' ? g_ota.error : "cannot start an update");
    }
    uint8_t *buffer = static_cast<uint8_t *>(malloc(kChunk));
    esp_err_t err = buffer != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
    if (buffer == nullptr) {
        ota::Abort("out of memory");
    }
    size_t left = request->content_len;
    int stalls = 0;
    while (err == ESP_OK && left > 0) {
        const int got = httpd_req_recv(request, reinterpret_cast<char *>(buffer),
                                       left < kChunk ? left : kChunk);
        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < kMaxStalls) {
            continue;
        }
        if (got <= 0) {
            ota::Abort("the upload was cut short");
            err = ESP_FAIL;
            break;
        }
        stalls = 0;
        left -= static_cast<size_t>(got);
        err = ota::Write(buffer, static_cast<size_t>(got));
    }
    free(buffer);
    if (err == ESP_OK) {
        err = ota::End();
    }
    ota::GetStatus(&g_ota);
    if (err != ESP_OK) {
        return RefuseAndClose(request, "422 Unprocessable Entity", g_ota.error);
    }
    size_t size = 0;
    char *json = Scratch(&size);
    std::snprintf(json, size, "{\"ok\":true,\"version\":\"%s\",\"restarting\":true}",
                  Esc(0, g_ota.job_version));
    SendJson(request, nullptr, json);
    ESP_LOGW(TAG, "firmware uploaded over http, restarting");
    vTaskDelay(pdMS_TO_TICKS(500));  // let the answer leave
    ota::Restart();
    return ESP_OK;
}

esp_err_t SdPost(httpd_req_t *request) {
    esp_err_t answer = ESP_OK;
    if (!MayUpdate(request, &answer)) {
        return answer;
    }
    ota::ReadSdFile(&g_sd);
    if (!g_sd.present) {
        return Refuse(request, "404 Not Found", "no firmware.bin in the card's root");
    }
    if (g_sd.verdict != ota::Verdict::kMore) {
        return Refuse(request, "422 Unprocessable Entity", ota::VerdictName(g_sd.verdict));
    }
    const esp_err_t err = ota::StartFromSd();
    if (err != ESP_OK) {
        return Refuse(request, "409 Conflict", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "firmware update from the sd card asked for over http");
    return SendJson(request, nullptr, "{\"ok\":true,\"started\":true}");
}

}  // namespace

esp_err_t RegisterFirmwareRoutes(httpd_handle_t server) {
    const httpd_uri_t routes[] = {
        {.uri = "/api/firmware", .method = HTTP_GET, .handler = FirmwareGet, .user_ctx = nullptr},
        {.uri = "/api/firmware/upload", .method = HTTP_POST, .handler = UploadPost,
         .user_ctx = nullptr},
        {.uri = "/api/firmware/sd", .method = HTTP_POST, .handler = SdPost, .user_ctx = nullptr},
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
