#pragma once

#include <cstddef>

#include "esp_err.h"

// Wi-Fi station: joins one saved network and keeps it (reconnects on loss).
// Used for syncing data and configuring the device. Off at boot.
//
// Credentials live in NVS (namespace "wifi"), not in the driver's own store
// (WIFI_STORAGE_RAM). NVS is not encrypted yet: the password is stored in
// plain text until the encrypted storage exists.

namespace wifi {

constexpr size_t kSsidSize = 33;      // 32 bytes + NUL
constexpr size_t kPasswordSize = 65;  // 64 (WPA PSK hex) + NUL

// Starts the radio and connects to the saved network, if any.
esp_err_t On();

// Disconnects and stops the radio; frees the driver's memory.
esp_err_t Off();

bool IsOn();

// True when associated and holding an IPv4 address.
bool Connected();

// True when a network is saved.
bool HasNetwork();

// Saves the network (empty password = open network). Reconnects if on.
esp_err_t SetCredentials(const char *ssid, const char *password);

// Deletes the saved network and disconnects.
esp_err_t ForgetCredentials();

// State, last failure, MAC, SSID/BSSID, channel, RSSI, IPv4 settings.
void PrintInfo();

// Scans (blocking, a few seconds) and prints the access points found.
// Requires the radio on.
esp_err_t Scan();

}  // namespace wifi
