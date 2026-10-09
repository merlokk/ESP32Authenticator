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
// Switching hosts: UseBond() picks the target bond. The current link is
// dropped (after drop listeners released held keys) and advertising is
// filtered to the target (controller filter accept list), so only it
// reconnects. The target is kept in NVS. A pairing window advertises to all.
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

// Called right before the device drops a link on purpose (switch, disconnect,
// unpair, off), from the task doing it. ble_kb releases held keys here.
using DropListener = void (*)(uint16_t conn);
void AddDropListener(DropListener listener);

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

// Deletes one bond, by number from PrintBonds() ("1") or by address
// ("aa:bb:cc:dd:ee:ff"), or all of them (`which` == nullptr). A connected host
// is disconnected.
esp_err_t Unpair(const char *which);

// Makes a bond the target host (number from PrintBonds() or address), or
// clears the target (`which` == nullptr: any bonded host may connect).
// Drops the current link if it is not the target.
esp_err_t UseBond(const char *which);

// Drops the current link (keys released first). The host may reconnect.
esp_err_t Disconnect();

// Current connection, BLE_HS_CONN_HANDLE_NONE if none.
uint16_t ConnHandle();

// True when the link is encrypted and authenticated.
bool LinkSecure();

// Usable ATT payload for notifications on the current link (MTU - 3).
uint16_t NotifyPayload();

// State, address, advertising, pairing window, target, connection and bonds
// summary.
void PrintInfo();

// Numbered bond list (numbers are what Unpair/UseBond take); marks the
// connected and the target host.
void PrintBonds();

// Current connection: peer, security, MTU, parameters, RSSI.
void PrintConnection();

}  // namespace ble
