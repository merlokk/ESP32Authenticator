#pragma once

// Device configuration: `config.json`, parsed into one fixed-size struct.
//
// - Init() reads `<dir>/config.json`. A missing, oversized or unparseable file
//   is not fatal: the built-in default config (kDefaultJson) is used and the
//   reason is kept in LastError(). Nothing is written at boot.
// - Save() writes atomically: `config.json.new`, then remove + rename (SPIFFS
//   rename does not replace). Init() finishes an interrupted write.
// - Missing fields take their default; unknown fields are ignored (and lost on
//   the next Save). An invalid value rejects the whole file.
// - Parse/Serialize/FillDefaults and the network helpers are pure (no files),
//   which is what the host tests exercise.
//
// Plain text on the filesystem, Wi-Fi passwords included, until the encrypted
// storage exists. Library layer: knows a file and its fields, no hardware.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace config {

constexpr const char *kFileName = "config.json";
constexpr const char *kTempName = "config.json.new";
constexpr size_t kMaxFileSize = 4096;
constexpr size_t kMaxDirSize = 32;

constexpr size_t kMaxNetworks = 4;
constexpr size_t kSsidSize = 33;      // 32 bytes + NUL
constexpr size_t kPasswordSize = 65;  // 8..63 chars, or 64 hex digits + NUL
constexpr size_t kHostnameSize = 33;
constexpr size_t kErrorSize = 96;

struct Network {
    char ssid[kSsidSize];
    char password[kPasswordSize];  // empty: open network
};

struct Wifi {
    bool active;  // start the station at boot
    char hostname[kHostnameSize];
    Network networks[kMaxNetworks];  // tried in order
    uint8_t network_count;
};

struct Ble {
    bool active;  // start BLE at boot
};

struct Data {
    Wifi wifi;
    Ble ble;
};

enum class Source : uint8_t {
    kDefault,  // built-in defaults (no usable config.json)
    kFile,     // config.json
};

// The built-in default config. Parsed by FillDefaults(), so it is checked by
// the same code (and tests) as a file.
extern const char kDefaultJson[];

// --- Pure -------------------------------------------------------------------

// Fills `out` with the built-in defaults.
void FillDefaults(Data *out);

// Parses `json` (`length` bytes, need not be NUL-terminated) over the defaults.
// On failure returns false, leaves `out` unchanged and writes a reason.
bool Parse(const char *json, size_t length, Data *out, char *error, size_t error_size);

// Writes `data` as pretty JSON. Returns the length, or 0 if it does not fit.
size_t Serialize(const Data &data, char *out, size_t size);

// Adds a network, or updates the password of one with the same SSID.
// Returns false (with a reason) on a bad SSID/password or a full list.
bool SetNetwork(Wifi *wifi, const char *ssid, const char *password, char *error,
                size_t error_size);

// Removes the network with `ssid`. Returns false if there is none.
bool RemoveNetwork(Wifi *wifi, const char *ssid);

// --- File -------------------------------------------------------------------

// Loads `<dir>/config.json` (dir: mounted filesystem, e.g. "/spiffs"), or the
// defaults if it is missing or invalid. Always leaves usable values; returns
// ESP_OK when the file was used.
esp_err_t Init(const char *dir);

// Re-reads the file. On failure keeps the current values.
esp_err_t Reload();

// Writes the current values to `<dir>/config.json`, atomically.
esp_err_t Save();

// The live values; change a field, then Save().
Data &Get();

Source LoadedFrom();

// Full path of the config file ("" before Init).
const char *Path();

// Why the file was not used by the last Init/Reload ("" if it was).
const char *LastError();

}  // namespace config
