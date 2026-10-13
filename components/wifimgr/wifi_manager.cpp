#include "wifi_manager.h"

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ping/ping_sock.h"

namespace wifimgr {

namespace {

constexpr const char *TAG = "wifimgr";

static_assert(kMaxNetworks == config::kMaxNetworks, "policy bitmask vs config list");
static_assert(wifi::kSsidSize == config::kSsidSize);
static_assert(wifi::kPasswordSize == config::kPasswordSize);
static_assert(wifi::kHostnameSize == config::kHostnameSize);
static_assert(kMaxProbeTargets == config::kMaxProbeTargets, "probe list vs config list");

constexpr uint32_t kStackBytes = 4096;
StackType_t task_stack[kStackBytes];
StaticTask_t task_storage;
TaskHandle_t task_handle = nullptr;

StaticSemaphore_t lock_storage;
SemaphoreHandle_t lock = nullptr;

wifi::Radio radio;
Policy policy;
Reachability reach;

// --- the ICMP echo ------------------------------------------------------------
// esp_ping runs a session on its own task and answers through callbacks: a
// small mailbox. The decisions are all in reachability.h.

struct PingMailbox {
    volatile bool done;
    volatile bool replied;
};
PingMailbox ping_mail = {};
esp_ping_handle_t ping_handle = nullptr;
uint32_t ping_started_ms = 0;
uint32_t ping_deadline_ms = 0;

void OnPingReply(esp_ping_handle_t, void *) { ping_mail.replied = true; }
void OnPingEnd(esp_ping_handle_t, void *) { ping_mail.done = true; }

void ClosePing() {
    if (ping_handle == nullptr) {
        return;
    }
    esp_ping_stop(ping_handle);
    esp_ping_delete_session(ping_handle);
    ping_handle = nullptr;
}

// One echo; a session per probe (esp_ping cannot retarget a live one).
bool StartPing(uint32_t address, uint32_t timeout_ms, uint32_t now_ms) {
    if (ping_handle != nullptr) {
        return false;
    }
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 1;
    config.timeout_ms = timeout_ms;
    config.data_size = 32;
    config.task_stack_size = 3072;
    config.target_addr.type = IPADDR_TYPE_V4;  // lwIP's ip_addr_t
    config.target_addr.u_addr.ip4.addr = address;
    esp_ping_callbacks_t callbacks = {};
    callbacks.on_ping_success = &OnPingReply;
    callbacks.on_ping_end = &OnPingEnd;
    ping_mail.done = false;
    ping_mail.replied = false;
    if (esp_ping_new_session(&config, &callbacks, &ping_handle) != ESP_OK) {
        ping_handle = nullptr;
        return false;
    }
    if (esp_ping_start(ping_handle) != ESP_OK) {
        ClosePing();
        return false;
    }
    ping_started_ms = now_ms;
    // Our own deadline on top of the library's: a session that never calls
    // back must not freeze the check.
    ping_deadline_ms = timeout_ms + 3000;
    return true;
}

// Edges already reported; cleared by every Start (the only new attempt).
bool reported_connected = false;
bool reported_failed = false;

// An action the radio was too busy for (a scan): kept, retried next pass.
Action pending = Action::kNone;

bool radio_ready = false;
esp_err_t radio_error = ESP_OK;
bool started = false;

TickHandler tick_handler = nullptr;
void *tick_user = nullptr;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

Failure Translate(wifi::Failure failure) {
    switch (failure) {
        case wifi::Failure::kAuth: return Failure::kAuth;
        case wifi::Failure::kNotFound: return Failure::kNotFound;
        default: return Failure::kOther;
    }
}

// The address text of config.json to the driver's binary form. config has
// already checked it; a disagreement falls back to DHCP, never half an address.
wifi::StaticIp AddressFor(const config::Network &network) {
    wifi::StaticIp ip;
    if (!network.ip.enabled) {
        return ip;
    }
    if (!config::ParseIpv4(network.ip.address, &ip.address) ||
        !config::ParseIpv4(network.ip.netmask, &ip.netmask) ||
        !config::ParseIpv4(network.ip.gateway, &ip.gateway)) {
        ESP_LOGE(TAG, "'%s': static address does not parse, using dhcp", network.ssid);
        return wifi::StaticIp{};
    }
    config::ParseIpv4(network.ip.dns1, &ip.dns1);  // optional: 0 stays 0
    config::ParseIpv4(network.ip.dns2, &ip.dns2);
    ip.enabled = true;
    return ip;
}

ProbeSettings ProbesFromConfig() {
    const config::InternetCheck &net = config::Get().internet;
    ProbeSettings settings;
    settings.enabled = net.check;
    settings.target_count = net.target_count;
    settings.interval_ms = static_cast<uint32_t>(net.interval_seconds) * 1000u;
    settings.failures_before_offline = net.failures;
    return settings;
}

Settings SettingsFromConfig() {
    const config::Wifi &w = config::Get().wifi;
    Settings settings;
    settings.network_count = w.network_count;
    settings.rounds_before_ap = w.rounds;
    settings.ap_window_ms = static_cast<uint32_t>(w.ap_window_seconds) * 1000u;
    return settings;
}

bool EnsureRadio() {
    if (radio_ready) {
        return true;
    }
    if (radio_error != ESP_OK) {
        return false;  // logged once
    }
    const esp_err_t err = radio.Init();
    if (err != ESP_OK) {
        radio_error = err;
        ESP_LOGE(TAG, "radio init failed (%s); wi-fi stays off", esp_err_to_name(err));
        return false;
    }
    radio.SetHostname(config::Get().wifi.hostname);
    radio_ready = true;
    return true;
}

// Carries out one action. False: the radio is busy (a scan), ask again.
bool ApplyAction(Action action, uint32_t now_ms) {
    const config::Wifi &w = config::Get().wifi;
    switch (action) {
        case Action::kNone:
            return true;

        case Action::kStop:
            if (radio_ready) {
                radio.Stop();
            }
            reported_connected = false;
            reported_failed = false;
            return true;

        case Action::kStartAp: {
            if (!EnsureRadio()) {
                return true;
            }
            const esp_err_t err = radio.StartAp(w.ap_ssid, w.ap_password, w.ap_channel);
            if (err == ESP_ERR_INVALID_STATE) {
                return false;
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "access point '%s': %s", w.ap_ssid, esp_err_to_name(err));
            }
            reported_connected = false;
            reported_failed = false;
            return true;
        }

        case Action::kStartClient: {
            if (!EnsureRadio()) {
                return true;
            }
            const uint8_t index = policy.Network();
            if (index >= w.network_count) {
                policy.OnFailed(Failure::kOther, now_ms);  // the list shrank
                return true;
            }
            const config::Network &network = w.networks[index];
            const bool keep_ap = policy.GetDesired() == Desired::kApSta;
            const wifi::StaticIp ip = AddressFor(network);
            const esp_err_t err = radio.StartClient(network.ssid, network.password, keep_ap, &ip);
            if (err == ESP_ERR_INVALID_STATE) {
                return false;  // not logged: retried every pass while a scan runs
            }
            // The SSID is logged, the password never.
            ESP_LOGI(TAG, "joining '%s' (network %u, round %u, %s)", network.ssid,
                     static_cast<unsigned>(index + 1), static_cast<unsigned>(policy.Round() + 1),
                     ip.enabled ? network.ip.address : "dhcp");
            reported_connected = false;
            reported_failed = false;
            if (err != ESP_OK) {
                policy.OnFailed(Failure::kOther, now_ms);
            }
            return true;
        }
    }
    return true;
}

// The internet check, on the same pass as everything else.
void PumpReachability(uint32_t now, bool link_is_up) {
    if (!link_is_up) {
        reach.LinkDown(now);
        ClosePing();  // its answer would be about nothing
        return;
    }
    reach.LinkUp(now);
    if (ping_handle != nullptr) {
        if (ping_mail.done) {
            const bool replied = ping_mail.replied;
            ClosePing();
            reach.OnResult(replied, now);
        } else if (now - ping_started_ms >= ping_deadline_ms) {
            ESP_LOGW(TAG, "ping session did not finish; counted as no answer");
            ClosePing();
            reach.OnResult(false, now);
        }
        return;
    }
    if (reach.Tick(now) != Probe::kSend) {
        return;
    }
    const config::InternetCheck &net = config::Get().internet;
    const uint8_t index = reach.Target();
    uint32_t address = 0;
    if (index >= net.target_count || !config::ParseIpv4(net.targets[index], &address) ||
        !StartPing(address, net.timeout_ms, now)) {
        reach.OnResult(false, now);  // the list changed under the round, or no session
    }
}

void Pump() {
    const uint32_t now = NowMs();
    bool link_is_up = false;
    if (radio_ready) {
        const wifi::Status status = radio.Get();
        policy.OnApClients(status.clients, now);
        if (status.link == wifi::Link::kConnected && !reported_connected) {
            reported_connected = true;
            policy.OnConnected(now);
        } else if (status.link == wifi::Link::kFailed && !reported_failed) {
            reported_failed = true;
            policy.OnFailed(Translate(status.failure), now);
        }
        // A client link with an address: an AP has no uplink of its own to test.
        link_is_up = (status.mode == wifi::Mode::kClient || status.mode == wifi::Mode::kApSta) &&
                     status.link == wifi::Link::kConnected && status.ip != 0;
    }
    PumpReachability(now, link_is_up);
    // Idempotent, so an edited file takes effect without anybody telling us.
    policy.SetDesired(DesiredFromConfig(), now);

    if (pending == Action::kNone) {
        pending = policy.Tick(now);
    }
    if (pending != Action::kNone && ApplyAction(pending, now)) {
        pending = Action::kNone;
    }
}

void Task(void *) {
    for (;;) {
        // Before the pump, outside the lock: the handler must let go of its
        // socket while the netif still exists (closing one after the radio
        // destroyed it is a null call), and it calls Get().
        if (tick_handler != nullptr) {
            tick_handler(tick_user);
        }
        if (xSemaphoreTake(lock, pdMS_TO_TICKS(kPollMs)) == pdTRUE) {
            Pump();
            xSemaphoreGive(lock);
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

Desired DesiredFromConfig() {
    const config::Wifi &w = config::Get().wifi;
    if (!w.active) {
        return Desired::kOff;
    }
    switch (w.mode) {
        case config::WifiMode::kAp: return Desired::kAp;
        case config::WifiMode::kApSta: return Desired::kApSta;
        case config::WifiMode::kClient: break;
    }
    return Desired::kClient;
}

void OnTick(TickHandler handler, void *user) {
    tick_handler = handler;
    tick_user = user;
}

esp_err_t Init() {
    if (started) {
        return ESP_OK;
    }
    lock = xSemaphoreCreateMutexStatic(&lock_storage);
    if (lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    policy.Configure(SettingsFromConfig(), NowMs());
    reach.Configure(ProbesFromConfig(), NowMs());
    task_handle = xTaskCreateStatic(&Task, "wifimgr", kStackBytes, nullptr, 4, task_stack,
                                    &task_storage);
    if (task_handle == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    started = true;
    ESP_LOGI(TAG, "config asks for %s, %u network(s)", Name(DesiredFromConfig()),
             static_cast<unsigned>(config::Get().wifi.network_count));
    return ESP_OK;
}

bool Ready() { return started; }

void Apply() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    pending = Action::kNone;
    if (radio_ready) {
        radio.SetHostname(config::Get().wifi.hostname);
    }
    policy.SetDesired(DesiredFromConfig(), NowMs());
    policy.Configure(SettingsFromConfig(), NowMs());
    reach.Configure(ProbesFromConfig(), NowMs());
    xSemaphoreGive(lock);
}

void ApplyInternetCheck() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    // Not policy.Configure: the ping list is no reason to drop a link.
    reach.Configure(ProbesFromConfig(), NowMs());
    xSemaphoreGive(lock);
}

void CheckInternetNow() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    reach.ProbeNow(NowMs());
    xSemaphoreGive(lock);
}

Snapshot Get() {
    Snapshot snapshot;
    if (!started) {
        return snapshot;
    }
    const uint32_t now = NowMs();
    xSemaphoreTake(lock, portMAX_DELAY);
    snapshot.desired = policy.GetDesired();
    snapshot.state = policy.GetState();
    snapshot.failure = policy.LastFailure();
    snapshot.network = policy.Network();
    snapshot.round = policy.Round();
    for (uint8_t i = 0; i < kMaxNetworks; ++i) {
        snapshot.auth_failed[i] = policy.AuthFailed(i);
    }
    snapshot.resting = policy.Resting();
    snapshot.ap_window_remaining_ms = policy.ApWindowRemainingMs(now);
    snapshot.wait_remaining_ms = policy.WaitRemainingMs(now);
    snapshot.radio_ready = radio_ready;
    snapshot.radio_error = radio_error;
    snapshot.internet = reach.State();
    snapshot.internet_failed_rounds = reach.FailedRounds();
    snapshot.internet_last_ok_ms = reach.SinceLastSuccessMs(now);
    snapshot.internet_next_probe_ms = reach.NextProbeInMs(now);
    snapshot.internet_target = reach.Target();
    snapshot.internet_probing = reach.Probing();
    xSemaphoreGive(lock);
    // Outside the lock: the radio takes its own.
    if (radio_ready) {
        snapshot.radio = radio.Get();
        snapshot.stack_up = radio.StackUp();
    }
    return snapshot;
}

esp_err_t Scan(wifi::ScanResult *out, size_t capacity, size_t *found) {
    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool have_radio = EnsureRadio();
    xSemaphoreGive(lock);
    if (!have_radio) {
        return radio_error != ESP_OK ? radio_error : ESP_ERR_INVALID_STATE;
    }
    // Not under the manager lock: it blocks for seconds. The manager sees
    // StartClient refused meanwhile and asks again.
    return radio.Scan(out, capacity, found);
}

}  // namespace wifimgr
