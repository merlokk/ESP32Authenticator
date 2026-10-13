#pragma once

// Owns the radio and the policy, the only place the two meet: one task polls
// wifi::Radio every kPollMs, turns what it reports into policy events and
// carries out the policy's actions with the SSIDs and passwords from
// config.json. The radio is brought up lazily (off costs nothing).

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "reachability.h"
#include "wifi_policy.h"
#include "wifi_radio.h"

namespace wifimgr {

constexpr uint32_t kPollMs = 200;

struct Snapshot {
    Desired desired = Desired::kOff;
    State state = State::kOff;
    Failure failure = Failure::kNone;
    uint8_t network = kNoNetwork;  // index into config wifi.networks
    uint8_t round = 0;
    bool auth_failed[kMaxNetworks] = {};
    bool resting = false;
    uint32_t ap_window_remaining_ms = 0;  // kHeldOpen when it does not run
    uint32_t wait_remaining_ms = 0;

    wifi::Status radio;  // what the driver says (may lag the policy a moment)
    bool radio_ready = false;
    // The TCP/IP stack exists. Anything opening a socket needs this:
    // listen() without lwIP is an assert, not an error.
    bool stack_up = false;
    esp_err_t radio_error = ESP_OK;

    // Is anything reachable through the client link (unknown without one).
    Internet internet = Internet::kUnknown;
    uint8_t internet_failed_rounds = 0;
    uint32_t internet_last_ok_ms = kNeverSucceeded;  // age, not a timestamp
    uint32_t internet_next_probe_ms = kNeverSucceeded;
    uint8_t internet_target = kNoTarget;  // index into config internet.targets
    bool internet_probing = false;
};

// Starts the task. The radio follows config.json from the first tick.
esp_err_t Init();
bool Ready();

// Re-reads config.json (networks, mode, AP, hostname) and restarts the
// current mode, dropping a working link. After any edit to wifi settings.
void Apply();

Snapshot Get();

// Re-reads the internet check settings only: the link is not touched.
void ApplyInternetCheck();

// Starts an internet check round now (nothing without a client link).
void CheckInternetNow();

// The desired mode config.json asks for (wifi.active false: off).
Desired DesiredFromConfig();

// Called on every pass of the manager task, before the pump and with no
// manager lock held (the handler may call Get()). One handler. The web
// server reconciles on it, so it lets go of its socket before the radio goes.
using TickHandler = void (*)(void *user);
void OnTick(TickHandler handler, void *user);

// Blocking scan; brings the radio up for it if off and puts it back. Never
// from the manager task.
esp_err_t Scan(wifi::ScanResult *out, size_t capacity, size_t *found);

}  // namespace wifimgr
