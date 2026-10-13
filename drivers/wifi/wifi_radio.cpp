#include "wifi_radio.h"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "soc/soc_caps.h"

// 2.4 GHz is every band the ESP32-S3 has; a dual-band part needs the band
// selected and reported.
#if defined(SOC_WIFI_SUPPORT_5G) && SOC_WIFI_SUPPORT_5G
#error "This chip has a 5 GHz radio: Scan() must select the band and report it."
#endif

namespace wifi {

namespace {

constexpr const char *TAG = "wifi";

wifi_ap_record_t scan_records[kMaxScanResults];

// Copies into a fixed field, refusing rather than truncating.
bool CopyField(char *out, size_t capacity, const char *in) {
    const size_t length = in == nullptr ? 0 : strlen(in);
    if (length >= capacity) {
        return false;
    }
    memcpy(out, in == nullptr ? "" : in, length + 1);
    return true;
}

}  // namespace

// --- names --------------------------------------------------------------------

Failure Radio::Classify(uint8_t reason) {
    switch (reason) {
        // A wrong WPA2 passphrase is mostly a handshake timeout, not AUTH_FAIL.
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return Failure::kAuth;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return Failure::kNotFound;
        default:
            return Failure::kOther;
    }
}

const char *Radio::Name(Mode mode) {
    switch (mode) {
        case Mode::kOff: return "off";
        case Mode::kClient: return "client";
        case Mode::kAp: return "ap";
        case Mode::kApSta: return "ap+client";
    }
    return "?";
}

const char *Radio::Name(Link link) {
    switch (link) {
        case Link::kIdle: return "idle";
        case Link::kConnecting: return "connecting";
        case Link::kConnected: return "connected";
        case Link::kFailed: return "failed";
    }
    return "?";
}

const char *Radio::Name(Failure failure) {
    switch (failure) {
        case Failure::kNone: return "none";
        case Failure::kAuth: return "wrong password";
        case Failure::kNotFound: return "no such network";
        case Failure::kOther: return "disconnected";
    }
    return "?";
}

const char *Radio::AuthName(uint8_t auth) {
    switch (static_cast<wifi_auth_mode_t>(auth)) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "wep";
        case WIFI_AUTH_WPA_PSK: return "wpa";
        case WIFI_AUTH_WPA2_PSK: return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
        case WIFI_AUTH_WPA3_PSK: return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
        default: return "other";
    }
}

// --- bring-up -----------------------------------------------------------------

esp_err_t Radio::Init() {
    if (ready_) {
        return ESP_OK;
    }
    lock_ = xSemaphoreCreateMutexStatic(&lock_storage_);
    if (lock_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    // NVS is initialised by main (esp_wifi keeps its PHY calibration there).
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // may exist already
        return err;
    }
    // Attached to the loop, not the driver: they survive the stack going down.
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventTrampoline, this,
                                              nullptr);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventTrampoline,
                                                  this, nullptr);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event handlers: %s", esp_err_to_name(err));
        return err;
    }
    ready_ = true;
    return ESP_OK;
}

void Radio::SetHostname(const char *hostname) {
    CopyField(hostname_, sizeof hostname_, hostname);
}

esp_err_t Radio::EnsureStack() {
    if (stack_ready_) {
        return ESP_OK;
    }
    // The default netifs cannot be created twice, so they live and die with
    // the stack, and must exist before esp_wifi_init.
    sta_netif_ = esp_netif_create_default_wifi_sta();
    ap_netif_ = esp_netif_create_default_wifi_ap();
    if (sta_netif_ == nullptr || ap_netif_ == nullptr) {
        ReleaseStack();
        return ESP_FAIL;
    }
    esp_netif_set_hostname(static_cast<esp_netif_t *>(sta_netif_), hostname_);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        ReleaseStack();
        return err;
    }
    stack_ready_ = true;
    // Set after every esp_wifi_init: it lives in the stack just created.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ReleaseStack();
        return err;
    }
    return ESP_OK;
}

void Radio::ReleaseStack() {
    if (started_) {
        esp_wifi_stop();
        started_ = false;
    }
    if (stack_ready_) {
        esp_wifi_deinit();
        stack_ready_ = false;
    }
    if (ap_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
    if (sta_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(sta_netif_);
        sta_netif_ = nullptr;
    }
    ap_up_ = false;
    want_connect_ = false;
    suppress_disconnect_ = 0;  // nothing can post an event now
}

esp_err_t Radio::EnsureStarted() {
    if (started_) {
        return ESP_OK;
    }
    const esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }
    started_ = true;
    return ESP_OK;
}

void Radio::DropStation() {
    // Only a station that is connecting or connected posts a disconnect event.
    const Link link = status_.link;
    if (started_ && (link == Link::kConnecting || link == Link::kConnected)) {
        suppress_disconnect_ = static_cast<uint8_t>(suppress_disconnect_ + 1);
        esp_wifi_disconnect();
    }
    want_connect_ = false;
}

// --- what the radio is asked to do ------------------------------------------

esp_err_t Radio::StartClient(const char *ssid, const char *password, bool keep_ap) {
    if (!ready_ || scanning_) {
        return ESP_ERR_INVALID_STATE;  // a scan holds the radio: ask again
    }
    if (ssid == nullptr || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t config = {};
    if (!CopyField(reinterpret_cast<char *>(config.sta.ssid), sizeof config.sta.ssid, ssid) ||
        !CopyField(reinterpret_cast<char *>(config.sta.password), sizeof config.sta.password,
                   password)) {
        return ESP_ERR_INVALID_ARG;
    }
    // The minimum accepted authmode: open stays joinable; with a password
    // ESP-IDF raises it to WPA2 itself.
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    config.sta.pmf_cfg.capable = true;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.failure_retry_cnt = 0;  // retries are the manager's

    esp_err_t err = EnsureStack();
    if (err != ESP_OK) {
        return err;
    }
    DropStation();

    const bool ap = keep_ap && ap_up_;
    err = esp_wifi_set_mode(ap ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station config: %s", esp_err_to_name(err));
        return err;
    }
    ap_up_ = ap;

    xSemaphoreTake(lock_, portMAX_DELAY);
    status_.mode = ap ? Mode::kApSta : Mode::kClient;
    status_.link = Link::kConnecting;
    status_.failure = Failure::kNone;
    status_.reason = 0;
    status_.ip = 0;
    status_.rssi = 0;
    CopyField(status_.ssid, sizeof status_.ssid, ssid);
    if (!ap) {
        status_.ap_ssid[0] = '\0';
        status_.ap_ip = 0;
        status_.clients = 0;
    }
    ++status_.changes;
    xSemaphoreGive(lock_);

    if (!started_) {
        // esp_wifi_connect before STA_START fails: connect from the event.
        want_connect_ = true;
        err = EnsureStarted();
        if (err != ESP_OK) {
            want_connect_ = false;
            SetLink(Link::kFailed, Failure::kOther, 0);
        }
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect to '%s': %s", ssid, esp_err_to_name(err));
        SetLink(Link::kFailed, Failure::kOther, 0);
    }
    return err;
}

esp_err_t Radio::StartAp(const char *ssid, const char *password, uint8_t channel) {
    if (!ready_ || scanning_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == nullptr || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t password_length = password == nullptr ? 0 : strlen(password);
    if (password_length != 0 && password_length < kMinApPasswordLength) {
        // Refused, not silently opened.
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t config = {};
    if (!CopyField(reinterpret_cast<char *>(config.ap.ssid), sizeof config.ap.ssid, ssid) ||
        !CopyField(reinterpret_cast<char *>(config.ap.password), sizeof config.ap.password,
                   password)) {
        return ESP_ERR_INVALID_ARG;
    }
    config.ap.ssid_len = static_cast<uint8_t>(strlen(ssid));
    config.ap.channel = channel;
    config.ap.authmode = password_length == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 4;
    config.ap.pmf_cfg.required = false;

    esp_err_t err = EnsureStack();
    if (err != ESP_OK) {
        return err;
    }
    DropStation();

    // APSTA with an idle station: a scan needs the station interface.
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ap config: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);
    const uint32_t changes = status_.changes + 1;
    status_ = {};
    status_.mode = Mode::kAp;
    CopyField(status_.ap_ssid, sizeof status_.ap_ssid, ssid);
    status_.changes = changes;
    xSemaphoreGive(lock_);

    err = EnsureStarted();
    if (err == ESP_OK) {
        ap_up_ = true;
        ESP_LOGI(TAG, "access point '%s' on channel %u, %s", ssid, static_cast<unsigned>(channel),
                 password_length == 0 ? "open" : "wpa2");
    }
    return err;
}

esp_err_t Radio::Stop() {
    if (!ready_ || (!started_ && !stack_ready_)) {
        return ESP_OK;
    }
    DropStation();
    ReleaseStack();
    xSemaphoreTake(lock_, portMAX_DELAY);
    const uint32_t changes = status_.changes + 1;
    status_ = {};
    status_.changes = changes;
    xSemaphoreGive(lock_);
    return ESP_OK;
}

Status Radio::Get() const {
    // Radio calls first: the lock is never held across one.
    int8_t rssi = 0;
    bool have_rssi = false;
    if (started_ && status_.link == Link::kConnected) {
        wifi_ap_record_t record = {};
        if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) {
            rssi = record.rssi;
            have_rssi = true;
        }
    }
    uint8_t channel = 0;
    if (started_) {
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&channel, &second) != ESP_OK) {
            channel = 0;
        }
    }
    xSemaphoreTake(lock_, portMAX_DELAY);
    Status copy = status_;
    xSemaphoreGive(lock_);
    if (have_rssi) {
        copy.rssi = rssi;
    }
    copy.channel = channel;
    return copy;
}

esp_err_t Radio::Scan(ScanResult *out, size_t capacity, size_t *found) {
    if (out == nullptr || found == nullptr || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *found = 0;
    if (!ready_ || scanning_) {
        return ESP_ERR_INVALID_STATE;
    }
    // Set first: StartClient / StartAp refuse while it is true.
    scanning_ = true;

    // A connect attempt scans too, and the two collide: abandon it.
    const bool abandoned = status_.link == Link::kConnecting;
    if (abandoned) {
        DropStation();
    }

    // Raise only what is down, and put exactly that back.
    const bool stack_was_ours = !stack_ready_;
    const bool station_was_off = !started_;
    esp_err_t err = EnsureStack();
    if (err == ESP_OK && station_was_off) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            err = EnsureStarted();
        }
    }
    const auto put_back = [this, stack_was_ours, station_was_off]() {
        if (stack_was_ours) {
            ReleaseStack();
        } else if (station_was_off) {
            esp_wifi_stop();
            started_ = false;
        }
    };

    uint16_t number = kMaxScanResults;
    if (err == ESP_OK) {
        wifi_scan_config_t config = {};  // active, every channel
        err = esp_wifi_scan_start(&config, true);
        if (err == ESP_OK) {
            err = esp_wifi_scan_get_ap_records(&number, scan_records);
        }
    }
    put_back();
    if (abandoned) {
        SetLink(Link::kFailed, Failure::kOther, 0);
    }
    scanning_ = false;
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        return err;
    }
    // Strongest first, as esp_wifi returns them.
    const size_t limit = number < capacity ? number : capacity;
    for (size_t i = 0; i < limit; ++i) {
        CopyField(out[i].ssid, sizeof out[i].ssid,
                  reinterpret_cast<const char *>(scan_records[i].ssid));
        out[i].rssi = scan_records[i].rssi;
        out[i].channel = scan_records[i].primary;
        out[i].auth = static_cast<uint8_t>(scan_records[i].authmode);
        out[i].secured = scan_records[i].authmode != WIFI_AUTH_OPEN;
    }
    *found = limit;
    return ESP_OK;
}

// --- events -------------------------------------------------------------------

void Radio::EventTrampoline(void *arg, const char *base, int32_t id, void *data) {
    static_cast<Radio *>(arg)->OnEvent(base, id, data);
}

void Radio::SetLink(Link link, Failure failure, uint8_t reason) {
    xSemaphoreTake(lock_, portMAX_DELAY);
    status_.link = link;
    status_.failure = failure;
    status_.reason = reason;
    if (link != Link::kConnected) {
        status_.ip = 0;
    }
    ++status_.changes;
    xSemaphoreGive(lock_);
}

void Radio::OnEvent(const char *base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                if (want_connect_) {
                    want_connect_ = false;
                    const esp_err_t err = esp_wifi_connect();
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "connect on start: %s", esp_err_to_name(err));
                        SetLink(Link::kFailed, Failure::kOther, 0);
                    }
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                if (suppress_disconnect_ > 0) {
                    suppress_disconnect_ = static_cast<uint8_t>(suppress_disconnect_ - 1);
                    break;
                }
                const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(data);
                const uint8_t reason = event != nullptr ? event->reason : 0;
                ESP_LOGW(TAG, "'%s': %s (reason %u)", status_.ssid, Name(Classify(reason)),
                         static_cast<unsigned>(reason));
                SetLink(Link::kFailed, Classify(reason), reason);
                break;
            }

            case WIFI_EVENT_AP_START: {
                esp_netif_ip_info_t info = {};
                if (ap_netif_ != nullptr &&
                    esp_netif_get_ip_info(static_cast<esp_netif_t *>(ap_netif_), &info) ==
                        ESP_OK) {
                    xSemaphoreTake(lock_, portMAX_DELAY);
                    status_.ap_ip = info.ip.addr;
                    ++status_.changes;
                    xSemaphoreGive(lock_);
                }
                break;
            }

            case WIFI_EVENT_AP_STACONNECTED:
            case WIFI_EVENT_AP_STADISCONNECTED:
                xSemaphoreTake(lock_, portMAX_DELAY);
                if (id == WIFI_EVENT_AP_STACONNECTED && status_.clients < 0xFF) {
                    ++status_.clients;
                } else if (id == WIFI_EVENT_AP_STADISCONNECTED && status_.clients > 0) {
                    --status_.clients;
                }
                ++status_.changes;
                xSemaphoreGive(lock_);
                break;

            default:
                break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<const ip_event_got_ip_t *>(data);
        esp_ip4_addr_t address = {};
        if (event != nullptr) {
            address = event->ip_info.ip;
        }
        xSemaphoreTake(lock_, portMAX_DELAY);
        status_.link = Link::kConnected;
        status_.failure = Failure::kNone;
        status_.reason = 0;
        status_.ip = address.addr;
        ++status_.changes;
        xSemaphoreGive(lock_);
        ESP_LOGI(TAG, "connected to '%s', ip " IPSTR, status_.ssid, IP2STR(&address));
    }
}

}  // namespace wifi
