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
                                      : Page::kMain;
}

// The main page's rows, under the frontlight.
inline constexpr Page kRows[] = {Page::kWifi,     Page::kBluetooth, Page::kDisplay,
                                 Page::kSecurity, Page::kTime,      Page::kAbout};
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
};

constexpr Action PageAction(Page p) {
    return p == Page::kDisplay    ? Action::kPowerOff
           : p == Page::kSecurity ? Action::kLockNow
           : p == Page::kTime     ? Action::kSyncTime
           : p == Page::kAbout    ? Action::kRestart
           : p == Page::kFido     ? Action::kFidoReset
                                  : Action::kNone;
}

constexpr const char *ActionText(Action a) {
    return a == Action::kPowerOff  ? "Power off"
           : a == Action::kLockNow ? "Lock now"
           : a == Action::kSyncTime ? "Sync the time now"
           : a == Action::kRestart  ? "Restart"
           : a == Action::kFidoReset ? "Reset FIDO2"
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

// The frontlight's sliders: brightness 1..100 (0 is 1), warmth 0..100.
constexpr uint8_t Brightness(int32_t slider) {
    return static_cast<uint8_t>(slider < 1 ? 1 : slider > 100 ? 100 : slider);
}
constexpr uint8_t Warmth(int32_t slider) {
    return static_cast<uint8_t>(slider < 0 ? 0 : slider > 100 ? 100 : slider);
}

}  // namespace ui::settings
