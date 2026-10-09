#pragma once

#include <cstdint>

#include "esp_err.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

// BLE peripheral on NimBLE: one connection, advertising while idle, bonding
// with passkey (the device displays it, the host types it in).
//
// New hosts can pair only inside a pairing window (`StartPairing`); outside it
// any new pairing is rejected and only bonded hosts get a link. Links must be
// encrypted and authenticated (MITM-protected).
//
// Profile drivers (ble_kb, ble_fido) add their GATT services and event
// listeners before On(). The stack is off at boot.

namespace ble {

constexpr const char *kDeviceName = "ESP32 Auth";

// Registers a GATT service table (kept by pointer, must stay alive).
// Call before On(); takes effect on the next On().
void AddServices(const ble_gatt_svc_def *services);

// Called from the NimBLE host task for every GAP event.
using Listener = void (*)(const ble_gap_event *event);
void AddListener(Listener listener);

esp_err_t On();
esp_err_t Off();
bool IsOn();

// Opens the pairing window for `seconds`; it closes earlier on a new bond.
esp_err_t StartPairing(uint32_t seconds);
void StopPairing();
uint32_t PairingSecondsLeft();

// What happens inside the pairing window, in order: kPasskey (show it to the
// user), then kPaired or kFailed. The host may retry after kFailed.
struct PairingEvent {
    enum Type { kPasskey, kPaired, kFailed } type;
    uint32_t passkey;    // kPasskey
    char peer[18];       // kPaired: host identity address
    const char *reason;  // kFailed
};

// Waits for the next pairing event since the last StartPairing().
// Returns false on timeout.
bool WaitPairingEvent(PairingEvent *event, uint32_t timeout_ms);

// Deletes one bond ("aa:bb:cc:dd:ee:ff") or all of them (`addr` == nullptr).
esp_err_t Unpair(const char *addr);

// Current connection, BLE_HS_CONN_HANDLE_NONE if none.
uint16_t ConnHandle();

// True when the link is encrypted and authenticated.
bool LinkSecure();

// Usable ATT payload for notifications on the current link (MTU - 3).
uint16_t NotifyPayload();

// State, address, connection, pairing window, bonds.
void PrintInfo();

}  // namespace ble
