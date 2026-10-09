#include "wifi.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

namespace wifi {

namespace {

constexpr const char *TAG = "wifi";
constexpr const char *kNvsNamespace = "wifi";
constexpr const char *kNvsSsid = "ssid";
constexpr const char *kNvsPassword = "pass";
constexpr uint16_t kMaxScanResults = 20;
constexpr uint64_t kRetryDelayUs = 2000000;

enum class State { kOff, kIdle, kConnecting, kConnected, kFailed };

esp_netif_t *netif = nullptr;
esp_timer_handle_t retry_timer = nullptr;
bool handlers_registered = false;
volatile State state = State::kOff;
volatile uint8_t last_reason = 0;    // wifi_err_reason_t of the last disconnect
volatile bool stopping = false;      // Off() in progress
// Our own disconnects report asynchronously (reason STA_LEAVING); events
// until this time are ours and not failures.
volatile int64_t own_disconnect_until_us = 0;
volatile uint32_t reconnects = 0;
char ssid[kSsidSize] = {};
char password[kPasswordSize] = {};

const char *StateName(State s) {
    switch (s) {
        case State::kOff: return "off";
        case State::kIdle: return "on, no network saved";
        case State::kConnecting: return "connecting";
        case State::kConnected: return "connected";
        case State::kFailed: return "failed, retrying";
    }
    return "?";
}

// The reason codes a person can act on; the raw code is printed beside it.
const char *ReasonName(uint8_t reason) {
    switch (reason) {
        case 0: return "none";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return "wrong password";
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "network not found";
        case WIFI_REASON_BEACON_TIMEOUT: return "signal lost";
        case WIFI_REASON_ASSOC_LEAVE: return "disconnected";
        default: return "other";
    }
}

const char *AuthName(wifi_auth_mode_t mode) {
    switch (mode) {
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

void LoadCredentials() {
    ssid[0] = '\0';
    password[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = sizeof ssid;
    if (nvs_get_str(nvs, kNvsSsid, ssid, &len) != ESP_OK) {
        ssid[0] = '\0';
    }
    len = sizeof password;
    if (nvs_get_str(nvs, kNvsPassword, password, &len) != ESP_OK) {
        password[0] = '\0';
    }
    nvs_close(nvs);
}

// Applies the saved network to the driver and starts connecting.
void Connect() {
    if (ssid[0] == '\0') {
        state = State::kIdle;
        return;
    }
    wifi_config_t config = {};
    strlcpy(reinterpret_cast<char *>(config.sta.ssid), ssid, sizeof config.sta.ssid);
    strlcpy(reinterpret_cast<char *>(config.sta.password), password,
            sizeof config.sta.password);
    // Accept WPA2 and newer when a password is set; open only without one.
    config.sta.threshold.authmode = password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &config);
    state = State::kConnecting;
    esp_wifi_connect();
}

void OnEvent(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        Connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
        if (stopping || state == State::kOff ||
            esp_timer_get_time() < own_disconnect_until_us) {
            return;
        }
        last_reason = event->reason;
        ESP_LOGW(TAG, "disconnected: %s (%u)", ReasonName(event->reason), event->reason);
        if (ssid[0] == '\0') {
            state = State::kIdle;
            return;
        }
        // Keep trying, the network may come back; a timer, not a sleep, so
        // the shared event loop is not blocked.
        state = State::kFailed;
        reconnects = reconnects + 1;
        esp_timer_stop(retry_timer);
        esp_timer_start_once(retry_timer, kRetryDelayUs);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<ip_event_got_ip_t *>(data);
        state = State::kConnected;
        last_reason = 0;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void OnRetry(void *) {
    if (!stopping && state == State::kFailed) {
        esp_wifi_connect();
    }
}

// Disconnects on purpose: the resulting event is not a failure.
void OwnDisconnect() {
    own_disconnect_until_us = esp_timer_get_time() + 1000000;
    esp_timer_stop(retry_timer);
    esp_wifi_disconnect();
}

esp_err_t InitOnce() {
    if (netif != nullptr) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // may exist already
        return err;
    }
    netif = esp_netif_create_default_wifi_sta();
    if (netif == nullptr) {
        return ESP_FAIL;
    }
    esp_netif_set_hostname(netif, "esp32-auth");
    const esp_timer_create_args_t args = {.callback = OnRetry,
                                          .arg = nullptr,
                                          .dispatch_method = ESP_TIMER_TASK,
                                          .name = "wifi_retry",
                                          .skip_unhandled_events = true};
    esp_timer_create(&args, &retry_timer);
    if (!handlers_registered) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, OnEvent, nullptr,
                                            nullptr);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, OnEvent, nullptr,
                                            nullptr);
        handlers_registered = true;
    }
    return ESP_OK;
}

}  // namespace

esp_err_t On() {
    if (state != State::kOff) {
        return ESP_OK;
    }
    esp_err_t err = InitOnce();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif init: %s", esp_err_to_name(err));
        return err;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: %s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    LoadCredentials();
    stopping = false;
    reconnects = 0;
    last_reason = 0;
    state = State::kIdle;
    err = esp_wifi_start();  // STA_START connects to the saved network
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        state = State::kOff;
    }
    return err;
}

esp_err_t Off() {
    if (state == State::kOff) {
        return ESP_OK;
    }
    stopping = true;
    esp_timer_stop(retry_timer);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    state = State::kOff;
    stopping = false;
    return ESP_OK;
}

bool IsOn() { return state != State::kOff; }

bool Connected() { return state == State::kConnected; }

bool HasNetwork() {
    if (state == State::kOff) {
        LoadCredentials();
    }
    return ssid[0] != '\0';
}

esp_err_t SetCredentials(const char *new_ssid, const char *new_password) {
    if (new_ssid == nullptr || new_ssid[0] == '\0' || strlen(new_ssid) >= kSsidSize) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t pass_len = new_password != nullptr ? strlen(new_password) : 0;
    if (pass_len != 0 && (pass_len < 8 || pass_len >= kPasswordSize)) {
        return ESP_ERR_INVALID_ARG;  // WPA passphrase: 8..63 chars, or 64 hex
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, kNvsSsid, new_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, kNvsPassword, pass_len != 0 ? new_password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK || state == State::kOff) {
        return err;
    }
    // Reconnect with the new network.
    OwnDisconnect();
    LoadCredentials();
    last_reason = 0;
    reconnects = 0;
    Connect();
    return ESP_OK;
}

esp_err_t ForgetCredentials() {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(nvs, kNvsSsid);
    nvs_erase_key(nvs, kNvsPassword);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    ssid[0] = '\0';
    password[0] = '\0';
    if (state != State::kOff) {
        OwnDisconnect();
        state = State::kIdle;
    }
    return err;
}

void PrintInfo() {
    printf("state      %s\n", StateName(state));
    if (state == State::kOff) {
        LoadCredentials();
    }
    printf("network    %s\n", ssid[0] != '\0' ? ssid : "(none saved, 'wifi set <ssid> <pass>')");
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        printf("mac        %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5]);
    }
    if (state == State::kOff) {
        return;
    }
    if (last_reason != 0) {
        printf("last error %s (reason %u), %" PRIu32 " retr%s\n", ReasonName(last_reason),
               last_reason, reconnects, reconnects == 1 ? "y" : "ies");
    }
    wifi_ap_record_t ap = {};
    if (state == State::kConnected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("bssid      %02x:%02x:%02x:%02x:%02x:%02x\n", ap.bssid[0], ap.bssid[1],
               ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        printf("channel    %u\n", ap.primary);
        printf("rssi       %d dBm\n", ap.rssi);
        printf("auth       %s\n", AuthName(ap.authmode));
    }
    esp_netif_ip_info_t ip = {};
    if (state == State::kConnected && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        printf("ip         " IPSTR "\n", IP2STR(&ip.ip));
        printf("netmask    " IPSTR "\n", IP2STR(&ip.netmask));
        printf("gateway    " IPSTR "\n", IP2STR(&ip.gw));
        esp_netif_dns_info_t dns = {};
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            printf("dns        " IPSTR "\n", IP2STR(&dns.ip.u_addr.ip4));
        }
    }
    const char *hostname = nullptr;
    if (esp_netif_get_hostname(netif, &hostname) == ESP_OK && hostname != nullptr) {
        printf("hostname   %s\n", hostname);
    }
}

esp_err_t Scan() {
    if (state == State::kOff) {
        return ESP_ERR_INVALID_STATE;
    }
    // A scan while a connect attempt is scanning too fails; pause retries.
    const bool was_retrying = state == State::kConnecting || state == State::kFailed;
    if (was_retrying) {
        OwnDisconnect();
    }
    esp_err_t err = esp_wifi_scan_start(nullptr, true);
    if (was_retrying) {
        Connect();
    }
    if (err != ESP_OK) {
        return err;
    }
    static wifi_ap_record_t records[kMaxScanResults];
    uint16_t count = kMaxScanResults;
    uint16_t total = 0;
    esp_wifi_scan_get_ap_num(&total);
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        return err;
    }
    printf("rssi  ch  auth       ssid\n");
    for (uint16_t i = 0; i < count; ++i) {
        printf("%4d  %2u  %-9s  %s\n", records[i].rssi, records[i].primary,
               AuthName(records[i].authmode),
               records[i].ssid[0] != '\0' ? reinterpret_cast<const char *>(records[i].ssid)
                                          : "(hidden)");
    }
    printf("%u network(s)%s\n", total, total > count ? ", strongest shown" : "");
    return ESP_OK;
}

}  // namespace wifi
