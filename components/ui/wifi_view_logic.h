#pragma once

// The Wi-Fi window of Settings (x4-design.md): the switch, the state, the
// saved networks (a tap: connect / forget); "Add a network" scans the ones
// nearby (strongest first, a name once); a tap on one joins it (saved or
// open) or asks its password on the keyboard. The device fills View and
// Nearby (ui::WifiHooks). Pure (host-tested).

#include <cstddef>
#include <cstdint>

namespace ui::wifi_view {

inline constexpr size_t kSsidSize = 33;  // 32 bytes + NUL
inline constexpr size_t kPasswordSize = 65;
inline constexpr uint8_t kMaxSaved = 4;  // config::kMaxNetworks
inline constexpr uint8_t kMaxNearby = 9;  // rows on the page
inline constexpr int8_t kNone = -1;

struct Saved {
    char ssid[kSsidSize];
    bool refused;  // the password was refused
};

// What the window shows; drawn again when it changes.
struct View {
    bool active;     // config wifi.active
    char state[24];  // "online", "connecting", ...
    char detail[56]; // "192.168.1.20", the access point and its address
    uint8_t saved_count;
    Saved saved[kMaxSaved];
    int8_t joined;  // the saved network the link is on, kNone
};

struct Nearby {
    char ssid[kSsidSize];
    int8_t rssi;
    bool secured;
};

constexpr bool Same(const char *a, const char *b) {
    for (; *a != '\0' && *a == *b; ++a, ++b) {
    }
    return *a == *b;
}

constexpr bool Equal(const View &a, const View &b) {
    if (a.active != b.active || !Same(a.state, b.state) || !Same(a.detail, b.detail) ||
        a.saved_count != b.saved_count || a.joined != b.joined) {
        return false;
    }
    for (uint8_t i = 0; i < a.saved_count && i < kMaxSaved; ++i) {
        if (!Same(a.saved[i].ssid, b.saved[i].ssid) || a.saved[i].refused != b.saved[i].refused) {
            return false;
        }
    }
    return true;
}

// Signal bars, 0..4.
constexpr uint8_t Bars(int8_t rssi) {
    return rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
}

// A scan tidied in place: hidden ones dropped, a name once (its strongest),
// strongest first, at most `keep`. The count left.
constexpr size_t Tidy(Nearby *list, size_t n, size_t keep) {
    size_t out = 0;
    for (size_t i = 0; i < n; ++i) {
        if (list[i].ssid[0] == '\0') {
            continue;
        }
        size_t j = 0;
        for (; j < out && !Same(list[j].ssid, list[i].ssid); ++j) {
        }
        if (j < out) {
            if (list[i].rssi > list[j].rssi) {
                list[j].rssi = list[i].rssi;
                list[j].secured = list[i].secured;
            }
            continue;
        }
        list[out++] = list[i];
    }
    for (size_t i = 1; i < out; ++i) {  // insertion: a dozen at most
        const Nearby v = list[i];
        size_t j = i;
        for (; j > 0 && list[j - 1].rssi < v.rssi; --j) {
            list[j] = list[j - 1];
        }
        list[j] = v;
    }
    return out < keep ? out : keep;
}

// The saved network with this name, kNone.
constexpr int8_t SavedIndex(const View &v, const char *ssid) {
    for (uint8_t i = 0; i < v.saved_count && i < kMaxSaved; ++i) {
        if (Same(v.saved[i].ssid, ssid)) {
            return static_cast<int8_t>(i);
        }
    }
    return kNone;
}

// A tap on a network nearby.
enum class Join : uint8_t {
    kSaved,     // its password is kept: connect
    kOpen,      // no password: saved and joined
    kPassword,  // the keyboard first
};

constexpr Join OnTap(const View &v, const Nearby &n) {
    return SavedIndex(v, n.ssid) != kNone ? Join::kSaved : !n.secured ? Join::kOpen : Join::kPassword;
}

constexpr bool Hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// WPA2: 8..63 characters, or 64 hex digits (config::ValidPassword).
constexpr bool PasswordOk(const char *p) {
    size_t n = 0;
    bool hex = true;
    for (; p[n] != '\0'; ++n) {
        hex = hex && Hex(p[n]);
    }
    return (n >= 8 && n <= 63) || (n == 64 && hex);
}

}  // namespace ui::wifi_view
