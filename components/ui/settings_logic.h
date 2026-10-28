#pragma once

// Settings (x4-design.md), as on a phone: the frontlight on top (on / off,
// brightness, warmth), then a row per page (its summary on the right); a page
// lists "label  value" lines and may have one button. The device fills the
// texts (ui::SettingsHooks::describe: "label\tvalue\n" lines). Pure
// (host-tested).

#include <cstddef>
#include <cstdint>

namespace ui::settings {

enum class Page : uint8_t {
    kMain,
    kWifi,
    kBluetooth,
    kDisplay,  // Devices: power, display, USB, the touch
    kSecurity,
    kTime,
    kAbout,
    kWifiScan,      // Wi-Fi's "Add a network": the ones nearby
    kWifiPassword,  // a network's password, on the keyboard
    kBlePair,       // Bluetooth's "Pair a new host": the window, the passkey
    kTouch,         // Devices: the touch's calibration, five crosses
    kDisplayChoice, // Devices: one setting's choices
    kTimeSync,      // Date & time: how often to sync
    kTimeRegion,    // Date & time: the zone's region
    kTimeZone,      // Date & time: the region's cities
    kTimeSet,       // Date & time: the time by hand
    kSecurityLock,  // Security: the lock after wrong PINs
    kSecurityPin,   // Security: a PIN typed (change it, the wipe PIN)
    kWeb,           // Wi-Fi: the web site
    kWebMode,       // Web site: when it runs
    kWebLogin,      // Web site: the login on the keyboard
    kFido,          // Security: FIDO2 / U2F (its state, the keys, reset)
    kBleName,       // Bluetooth: the name on the keyboard
    kWifiMore,      // Wi-Fi: mode and access point
    kWifiChoice,    // Wi-Fi > Mode and access point: one setting's choices
    kWifiText,      // Wi-Fi > Mode and access point: a typed field
    kWifiStatic,    // Wi-Fi: a saved network's IP address
    kWifiStaticText,  // Wi-Fi > IP address: a typed address
    kWifiHidden,    // Wi-Fi > Add a network: one not broadcast (its name, its password)
    kServer,        // the authentication server: registered, connected, the code
    kServerEmail,   // Server > Register: the e-mail on the keyboard
    kServerConfig,  // Server > Configure: how often the status and the sync go
    kServerSync,    // Server > Sync: the stores against the server's, what waits
    kServerSyncPin, // Server > Sync: the PIN of the server's copy, on the keyboard
};

// Where a page's back arrow goes.
constexpr Page Parent(Page p) {
    return p == Page::kWifiScan || p == Page::kWifiMore || p == Page::kWifiStatic ||
                   p == Page::kWeb
               ? Page::kWifi
           : p == Page::kWifiChoice || p == Page::kWifiText ? Page::kWifiMore
           : p == Page::kWifiStaticText ? Page::kWifiStatic
           : p == Page::kWifiHidden   ? Page::kWifiScan
           : p == Page::kWifiPassword ? Page::kWifiScan
           : p == Page::kBlePair || p == Page::kBleName ? Page::kBluetooth
           : p == Page::kDisplayChoice || p == Page::kTouch ? Page::kDisplay
           : p == Page::kTimeSync || p == Page::kTimeRegion || p == Page::kTimeSet ? Page::kTime
           : p == Page::kTimeZone ? Page::kTimeRegion
           : p == Page::kSecurityLock || p == Page::kSecurityPin ? Page::kSecurity
           : p == Page::kWebMode || p == Page::kWebLogin ? Page::kWeb
           : p == Page::kFido ? Page::kSecurity
           : p == Page::kServerEmail || p == Page::kServerConfig || p == Page::kServerSync
               ? Page::kServer
           : p == Page::kServerSyncPin ? Page::kServerSync
                                      : Page::kMain;
}

// The main page's rows, under the frontlight.
inline constexpr Page kRows[] = {Page::kWifi,     Page::kBluetooth, Page::kDisplay,
                                 Page::kSecurity, Page::kServer,    Page::kTime,
                                 Page::kAbout};
inline constexpr uint8_t kRowCount = sizeof kRows / sizeof kRows[0];

constexpr const char *Title(Page p) {
    switch (p) {
        case Page::kMain: return "Settings";
        case Page::kWifi: return "Wi-Fi";
        case Page::kBluetooth: return "Bluetooth";
        case Page::kDisplay: return "Devices";
        case Page::kSecurity: return "Security";
        case Page::kTime: return "Date & time";
        case Page::kAbout: return "About";
        case Page::kWifiScan: return "Add a network";
        case Page::kWifiPassword: return "Password";
        case Page::kBlePair: return "Pair a new host";
        case Page::kTouch: return "Touchscreen";
        case Page::kDisplayChoice: return "Devices";
        case Page::kTimeSync: return "Sync";
        case Page::kTimeRegion: return "Time zone";
        case Page::kTimeZone: return "Time zone";
        case Page::kTimeSet: return "Set the time";
        case Page::kSecurityLock: return "Lock after wrong PINs";
        case Page::kSecurityPin: return "PIN";
        case Page::kWeb: return "Web site";
        case Page::kWebMode: return "Web site";
        case Page::kWebLogin: return "Login";
        case Page::kFido: return "FIDO2 / U2F";
        case Page::kBleName: return "Name";
        case Page::kWifiMore: return "Mode and access point";
        case Page::kWifiChoice: return "Wi-Fi";
        case Page::kWifiText: return "Wi-Fi";
        case Page::kWifiStatic: return "IP address";
        case Page::kWifiStaticText: return "IP address";
        case Page::kWifiHidden: return "Other network";
        case Page::kServer: return "Server";
        case Page::kServerEmail: return "E-mail";
        case Page::kServerConfig: return "Configure";
        case Page::kServerSync: return "Sync";
        case Page::kServerSyncPin: return "The copy's PIN";
    }
    return "";
}

// A page's button.
enum class Action : uint8_t {
    kNone,
    kPowerOff,  // Devices
    kLockNow,   // Security
    kSyncTime,  // Date & time
    kRestart,   // About (asked first: the vault locks)
    kFidoReset, // FIDO2 / U2F (held: every key and its PIN erased)
    kServerRegister,  // Server (asked first: a new key, then the e-mail)
    kServerStatus,    // Server > Configure: the status now
    kServerSync,      // Server > Sync: the sync now
};

constexpr Action PageAction(Page p) {
    return p == Page::kDisplay    ? Action::kPowerOff
           : p == Page::kSecurity ? Action::kLockNow
           : p == Page::kTime     ? Action::kSyncTime
           : p == Page::kAbout    ? Action::kRestart
           : p == Page::kFido     ? Action::kFidoReset
           : p == Page::kServer   ? Action::kServerRegister
           : p == Page::kServerConfig ? Action::kServerStatus
           : p == Page::kServerSync   ? Action::kServerSync
                                  : Action::kNone;
}

constexpr const char *ActionText(Action a) {
    return a == Action::kPowerOff  ? "Power off"
           : a == Action::kLockNow ? "Lock now"
           : a == Action::kSyncTime ? "Sync the time now"
           : a == Action::kRestart  ? "Restart"
           : a == Action::kFidoReset ? "Reset FIDO2"
           : a == Action::kServerRegister ? "Register"
           : a == Action::kServerStatus   ? "Send the status now"
           : a == Action::kServerSync     ? "Sync now"
                                    : "";
}

// Line `index` of "label\tvalue\n" lines into `label` / `value` (cut to
// their sizes; a line without a tab: all label). False past the last line.
constexpr bool Line(const char *text, uint8_t index, char *label, size_t label_size, char *value,
                    size_t value_size) {
    for (; index > 0 && *text != '\0'; ++text) {
        if (*text == '\n') {
            --index;
        }
    }
    if (*text == '\0') {
        return false;
    }
    size_t n = 0;
    for (; *text != '\0' && *text != '\n' && *text != '\t'; ++text) {
        if (n + 1 < label_size) {
            label[n++] = *text;
        }
    }
    label[n] = '\0';
    n = 0;
    if (*text == '\t') {
        for (++text; *text != '\0' && *text != '\n'; ++text) {
            if (n + 1 < value_size) {
                value[n++] = *text;
            }
        }
    }
    value[n] = '\0';
    return true;
}

// Server > Register's e-mail: 3..64 printable ASCII, no spaces, one '@' with
// something on both sides (config::ValidEmail, not empty).
inline constexpr size_t kEmailSize = 65;  // config::kEmailSize

constexpr bool EmailOk(const char *email) {
    size_t n = 0;
    size_t at = 0;
    size_t ats = 0;
    for (; email[n] != '\0'; ++n) {
        if (email[n] <= ' ' || email[n] > '~') {
            return false;
        }
        if (email[n] == '@') {
            at = n;
            ++ats;
        }
    }
    return n >= 3 && n < kEmailSize && ats == 1 && at > 0 && at + 1 < n;
}

// Server > Configure: the status report's and the sync's interval
// (config.json server.statusMinutes / syncMinutes, config::kStatusMinutes).
struct StatusChoice {
    uint8_t minutes;
    const char *label;
    const char *short_label;  // a button in a row of them
};
inline constexpr StatusChoice kStatusChoices[] = {{0, "Never", "never"},
                                                  {1, "Every minute", "1 min"},
                                                  {10, "Every 10 min", "10 min"},
                                                  {30, "Every 30 min", "30 min"},
                                                  {60, "Every hour", "1 hour"}};
// Configure's taps: a status choice is its row, a sync choice this plus it.
inline constexpr int kSyncChoiceBase = 10;
inline constexpr uint8_t kStatusChoiceCount = sizeof kStatusChoices / sizeof kStatusChoices[0];

// Its row in kStatusChoices; -1: none.
constexpr int StatusChoiceIndex(uint8_t minutes) {
    for (uint8_t i = 0; i < kStatusChoiceCount; ++i) {
        if (kStatusChoices[i].minutes == minutes) {
            return i;
        }
    }
    return -1;
}

// The Configure row's value on the Server page: "status every 10 min",
// "status never".
constexpr const char *StatusSummary(uint8_t minutes) {
    return minutes == 0    ? "status never"
           : minutes == 1  ? "status every min"
           : minutes == 10 ? "status every 10 min"
           : minutes == 30 ? "status every 30 min"
           : minutes == 60 ? "status every hour"
                           : "status";
}

// Server > Sync: which stores wait for a person (ServerHooks::sync_waits):
// bit 2i a conflict, bit 2i+1 the server's copy's PIN asked; i 0 the
// passwords, 1 the passkeys.
inline constexpr uint8_t kSyncStores = 2;
constexpr bool SyncConflict(uint8_t waits, uint8_t store) { return (waits >> (2 * store)) & 1; }
constexpr bool SyncPinAsked(uint8_t waits, uint8_t store) {
    return (waits >> (2 * store + 1)) & 1;
}
constexpr const char *SyncStoreTitle(uint8_t store) {
    return store == 0 ? "Passwords" : "Passkeys";
}
// The Sync page's taps: store * 2 + 0 keep this device's copy, + 1 take the
// server's (or type its PIN).
constexpr int SyncTap(uint8_t store, bool take) { return store * 2 + (take ? 1 : 0); }

// The frontlight's sliders: brightness 1..100 (0 is 1), warmth 0..100.
constexpr uint8_t Brightness(int32_t slider) {
    return static_cast<uint8_t>(slider < 1 ? 1 : slider > 100 ? 100 : slider);
}
constexpr uint8_t Warmth(int32_t slider) {
    return static_cast<uint8_t>(slider < 0 ? 0 : slider > 100 ? 100 : slider);
}

}  // namespace ui::settings
