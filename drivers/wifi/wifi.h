#pragma once

#include <cstddef>

#include "esp_err.h"

// Wi-Fi station. Joins one of the networks it is given, trying them in order
// (on a failed attempt it moves to the next), and keeps the link (reconnects
// on loss). Off at boot.
//
// The driver stores nothing: the network list and hostname come from the
// caller (main / the console, out of config.json).

namespace wifi {

constexpr size_t kSsidSize = 33;      // 32 bytes + NUL
constexpr size_t kPasswordSize = 65;  // 8..63 chars or 64 hex digits + NUL
constexpr size_t kMaxNetworks = 4;

struct Network {
    char ssid[kSsidSize];
    char password[kPasswordSize];  // empty: open network
};

// Replaces the network list (copied). If on, reconnects from the first one.
void SetNetworks(const Network *networks, size_t count);

// Hostname for DHCP; applied on the next On().
void SetHostname(const char *hostname);

// Starts the radio and connects to the first network, if any.
esp_err_t On();

// Disconnects and stops the radio; frees the driver's memory.
esp_err_t Off();

bool IsOn();

// True when associated and holding an IPv4 address.
bool Connected();

// State, network being tried, last failure, MAC, BSSID, channel, RSSI, IPv4.
void PrintInfo();

// Scans (blocking, a few seconds) and prints the access points found.
// Requires the radio on.
esp_err_t Scan();

}  // namespace wifi
