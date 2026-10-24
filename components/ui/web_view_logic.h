#pragma once

// The Web site window of Settings (x4-design.md): whether it runs and where
// (the address and a QR code of it), when it runs (web.mode), whether the
// site may change settings (web.write), the login (web.user / web.password:
// both or neither, typed on the keyboard, the user first). The device fills
// View and carries out the changes (ui::WebHooks). Pure (host-tested).

#include <cstddef>
#include <cstdint>

namespace ui::web_view {

// config::WebMode's order.
inline constexpr uint8_t kModeOff = 0;
inline constexpr uint8_t kModeOn = 1;
inline constexpr uint8_t kModeAuto = 2;

struct Option {
    uint8_t value;
    const char *label;
};

inline constexpr Option kModes[] = {{kModeOff, "Off"},
                                    {kModeOn, "On"},
                                    {kModeAuto, "On the device's access point"}};
inline constexpr uint8_t kModeCount = sizeof kModes / sizeof kModes[0];

constexpr const char *ModeLabel(uint8_t mode) {
    return mode == kModeOff ? "Off" : mode == kModeOn ? "On" : "Its access point";
}

inline constexpr size_t kUserSize = 33;      // config::kWebUserSize
inline constexpr size_t kPasswordSize = 65;  // config::kWebPasswordSize

// 1..32 characters, no ':' (basic auth splits there).
constexpr bool UserOk(const char *user) {
    size_t n = 0;
    for (; user[n] != '\0'; ++n) {
        if (user[n] == ':') {
            return false;
        }
    }
    return n >= 1 && n < kUserSize;
}

constexpr bool PasswordOk(const char *password) {
    size_t n = 0;
    for (; password[n] != '\0'; ++n) {
    }
    return n >= 1 && n < kPasswordSize;
}

// What the window shows; drawn again when it changes.
struct View {
    bool running;
    char state[48];  // "Running", "Stopped: Wi-Fi is off", ...
    char url[40];    // "http://192.168.1.20/" (the QR code), "" when none
    uint8_t mode;
    bool write;
    bool login;
    char user[kUserSize];
};

constexpr bool Same(const char *a, const char *b) {
    for (; *a != '\0' && *a == *b; ++a, ++b) {
    }
    return *a == *b;
}

constexpr bool Equal(const View &a, const View &b) {
    return a.running == b.running && Same(a.state, b.state) && Same(a.url, b.url) &&
           a.mode == b.mode && a.write == b.write && a.login == b.login && Same(a.user, b.user);
}

}  // namespace ui::web_view
