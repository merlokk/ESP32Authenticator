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
constexpr size_t kWebUserSize = 33;      // 32 chars + NUL, no ':'
constexpr size_t kWebPasswordSize = 65;  // 64 chars + NUL
constexpr size_t kErrorSize = 96;

constexpr size_t kIpTextSize = 16;  // "255.255.255.255" + NUL
constexpr size_t kPinSize = 7;       // 6 digits + NUL
constexpr size_t kWipeSize = 98;     // "<32 hex salt>:<64 hex SHA-256>" + NUL
constexpr uint16_t kMaxLockMinutes = 1440;
constexpr size_t kZoneSize = 41;     // zone name or POSIX rule (tz::kMaxPosixLength)
constexpr size_t kHostSize = 64;     // SNTP server: host name or IPv4
// BLE device name: 29 bytes fill the scan response (31 - length - type).
constexpr size_t kBleNameSize = 30;
constexpr uint8_t kMaxSyncHours = 168;
constexpr uint16_t kMaxIdleMinutes = 1440;
constexpr uint8_t kMaxKeepPinHours = 168;

// A fixed address for one network (per network: home DHCP, office static).
// Kept as typed; `enabled` false is DHCP and keeps the fields for later.
struct StaticIp {
    bool enabled;
    char address[kIpTextSize];
    char netmask[kIpTextSize];
    char gateway[kIpTextSize];
    char dns1[kIpTextSize];  // optional, empty: not set
    char dns2[kIpTextSize];
};

struct Network {
    char ssid[kSsidSize];
    char password[kPasswordSize];  // empty: open network
    StaticIp ip;
};

enum class WifiMode : uint8_t {
    kClient,  // join the networks in order; fallback AP window when none answers
    kAp,      // own access point only
    kApSta,   // own access point always, and join the networks too
};

struct Wifi {
    bool active;  // radio on at boot (and the on/off switch for `mode`)
    WifiMode mode;
    char hostname[kHostnameSize];
    Network networks[kMaxNetworks];  // tried in order
    uint8_t network_count;
    uint8_t rounds;              // client: full passes over the list before the fallback AP
    uint16_t ap_window_seconds;  // client: how long the fallback AP stays up with nobody on it
    char ap_ssid[kSsidSize];
    char ap_password[kPasswordSize];  // empty: open AP, else 8..63 chars
    uint8_t ap_channel;
};

constexpr size_t kMaxProbeTargets = 4;

// Internet check: while there is a client link, ping these (IPv4, no names:
// an ICMP echo has no resolver) every interval. A list, because many
// networks drop ICMP to one operator or another.
struct InternetCheck {
    bool check;
    uint16_t interval_seconds;  // 5..3600
    uint16_t timeout_ms;        // 100..10000, per echo
    uint8_t failures;           // consecutive failed rounds that mean offline, 1..10
    char targets[kMaxProbeTargets][kIpTextSize];
    uint8_t target_count;
};

struct Ble {
    bool active;                // start BLE at boot
    char name[kBleNameSize];    // advertised and GAP name; "": the board's
};

enum class WebMode : uint8_t {
    kOff,
    kOn,    // whenever there is a network
    kAuto,  // only while the device's own access point is up
};

struct Web {
    WebMode mode;
    bool write;  // false: settings and actions are refused (pages read-only)
    char user[kWebUserSize];  // basic auth when both user and password are set
    char password[kWebPasswordSize];
};

// The clock is UTC; `zone` is how a time is shown and how a typed time is read.
struct Time {
    char zone[kZoneSize];    // table name (Europe/Kyiv) or raw POSIX rule
    char server[kHostSize];  // empty: no sync
    uint8_t sync_hours;      // 0..168, 0: no sync
};

// Brightness ceilings of the board's LED, in percent (colours are compiled in).
struct Led {
    uint8_t percent;       // 0..100, every colour is scaled to it
    uint8_t idle_percent;  // 0..100, resting states (the ready breath)
};

struct Frontlight {
    uint8_t brightness;  // 1..100, the level On() lights (off at boot)
    uint8_t warmth;      // 0..100: 0 all cool, 100 all warm
    uint16_t off_minutes;      // 0..1440: off after this long idle (on again at a touch); 0: never
    uint16_t off_minutes_usb;  // the same on USB power; 0: never
};

// The power-off wallpaper: 4 grey levels (on a panel that has them, epd
// HasGrey; else B/W anyway) or B/W.
enum class WallpaperMode : uint8_t {
    kGrey,
    kBw,
};

// The touch's calibration (Settings > Touchscreen): screen = raw * scale /
// 1000 + offset, per axis; 1000 / 0 as it comes.
struct TouchCalibration {
    int16_t x_scale;   // 500..1500
    int16_t x_offset;  // -200..200 px
    int16_t y_scale;
    int16_t y_offset;
};

struct Display {
    WallpaperMode wallpaper;
    TouchCalibration touch;
};

// Power-off on its own, and the PIN through a sleep (components/power).
struct Power {
    uint16_t idle_minutes;   // 0..1440: off after this long with nothing done; 0: never
    uint16_t idle_minutes_usb;  // the same on USB power (a host, or charging); 0: never
    uint8_t keep_pin_hours;  // 0..168: a wake this soon after the sleep needs no PIN; 0: always
    bool hold_rail;          // the peripheral rail on through the sleep; off: on the battery
                             // the board's power drops (no drain, the kept PIN lost)
};

// The vault PIN (x4-design.md): the lock after 10 wrong PINs, and the wipe
// PIN, kept only as a salted hash (set by `pin wipe set`).
struct Pin {
    uint16_t lock_minutes;  // 1..1440
    char wipe[kWipeSize];   // empty: no wipe PIN
    bool complexity;        // a new PIN must pass pin_policy.h (vault and FIDO2)
};

// The USB port (devices/usb): the chip's built-in Serial/JTAG (the console,
// flashing and JTAG), or TinyUSB: a serial port with the console, with a
// FIDO2 / U2F key (CTAPHID) beside it. Switched at once, no restart.
enum class UsbMode : uint8_t {
    kJtag,        // "jtag": built-in USB Serial/JTAG
    kSerial,      // "serial": a CDC-ACM serial port
    kSerialFido,  // "serial+fido": the serial port and a FIDO2 / U2F key
};

struct Usb {
    UsbMode mode;
};

// Development and testing aids. Plain text in config.json: never on a
// device that holds real secrets.
struct Test {
    char pin[kPinSize];  // the vault PIN, entered at boot; empty: ask (`pin`)
    bool auto_presence;  // FIDO2/U2F user presence without the button (as if pressed)
};

struct Data {
    Wifi wifi;
    InternetCheck internet;
    Ble ble;
    Web web;
    Time time;
    Led led;
    Frontlight frontlight;
    Display display;
    Power power;
    Pin pin;
    Usb usb;
    Test test;
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

// Field rules, shared with the web settings write path.
bool ValidSsid(const char *ssid);          // 1..32 bytes
bool ValidPassword(const char *password);  // empty, 8..63 chars or 64 hex digits
bool ValidApPassword(const char *password);  // empty or 8..63 chars
bool ValidWebUser(const char *user);         // 0..32 chars, no ':'
bool ValidTestPin(const char *pin);          // empty or 6 digits
bool ValidWipe(const char *wipe);            // empty or "<32 hex>:<64 hex>"
bool ValidZone(const char *zone);            // a known zone name or a POSIX rule
bool ValidHost(const char *host);            // empty, or a host name / IPv4: a-z 0-9 '-' '.'
bool ValidBleName(const char *name);        // empty, or 1..29 bytes of UTF-8, no control chars

// Dotted quad to esp_netif's form (first octet in the low byte). Strict:
// four decimal octets 0..255, no leading zeros ("010" is octal elsewhere),
// nothing before or after.
bool ParseIpv4(const char *text, uint32_t *out);

// Checks a static block: with `enabled`, address/netmask/gateway required;
// every non-empty field must parse. False with a reason naming the field.
bool CheckStaticIp(const StaticIp &ip, char *error, size_t error_size);

// "client" / "ap" / "apsta" and "off" / "on" / "auto".
const char *Name(WifiMode mode);
bool ParseWifiMode(const char *text, WifiMode *out);
const char *Name(WebMode mode);
bool ParseWebMode(const char *text, WebMode *out);
const char *Name(WallpaperMode mode);
bool ParseWallpaperMode(const char *text, WallpaperMode *out);
const char *Name(UsbMode mode);
bool ParseUsbMode(const char *text, UsbMode *out);

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
