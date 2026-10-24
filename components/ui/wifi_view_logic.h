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
    // Mode and access point (config wifi.*).
    uint8_t mode;  // kModeClient ..
    char ap_name[kSsidSize];
    bool ap_password;  // set (else an open access point)
    uint8_t channel;
    uint8_t rounds;
    uint16_t window;  // seconds
    char hostname[kSsidSize];
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
        a.saved_count != b.saved_count || a.joined != b.joined || a.mode != b.mode ||
        !Same(a.ap_name, b.ap_name) || a.ap_password != b.ap_password || a.channel != b.channel ||
        a.rounds != b.rounds || a.window != b.window || !Same(a.hostname, b.hostname)) {
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

// --- Mode and access point: choices (config wifi.mode, ap.channel, rounds,
// apWindowSeconds)

inline constexpr uint8_t kModeClient = 0;  // config::WifiMode's order
inline constexpr uint8_t kModeAp = 1;
inline constexpr uint8_t kModeBoth = 2;

enum class Choice : uint8_t { kMode, kChannel, kRounds, kWindow };

struct Option {
    uint16_t value;
    const char *label;
};

inline constexpr Option kModes[] = {{kModeClient, "Join a network"},
                                    {kModeAp, "Its own access point"},
                                    {kModeBoth, "Both"}};
inline constexpr Option kChannels[] = {{1, "1"},  {2, "2"},  {3, "3"},  {4, "4"},  {5, "5"},
                                       {6, "6"},  {7, "7"},  {8, "8"},  {9, "9"},  {10, "10"},
                                       {11, "11"}, {12, "12"}, {13, "13"}};
inline constexpr Option kRoundsList[] = {
    {1, "After 1 round"}, {2, "After 2 rounds"}, {3, "After 3 rounds"}, {5, "After 5 rounds"},
    {10, "After 10 rounds"}};
inline constexpr Option kWindows[] = {{30, "30 s"},    {60, "1 min"},   {120, "2 min"},
                                      {300, "5 min"},  {600, "10 min"}, {1800, "30 min"},
                                      {3600, "1 hour"}};

constexpr const Option *Options(Choice c, size_t *count) {
    switch (c) {
        case Choice::kMode: *count = sizeof kModes / sizeof kModes[0]; return kModes;
        case Choice::kChannel: *count = sizeof kChannels / sizeof kChannels[0]; return kChannels;
        case Choice::kRounds: *count = sizeof kRoundsList / sizeof kRoundsList[0]; return kRoundsList;
        case Choice::kWindow: *count = sizeof kWindows / sizeof kWindows[0]; return kWindows;
    }
    *count = 0;
    return nullptr;
}

constexpr const char *ChoiceTitle(Choice c) {
    return c == Choice::kMode      ? "Wi-Fi mode"
           : c == Choice::kChannel ? "Access point channel"
           : c == Choice::kRounds  ? "The access point opens"
                                   : "It stays with nobody on it";
}

constexpr int ChoiceIndex(Choice c, uint16_t value) {
    size_t n = 0;
    const Option *o = Options(c, &n);
    for (size_t i = 0; i < n; ++i) {
        if (o[i].value == value) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

constexpr const char *ModeLabel(uint8_t mode) {
    return mode == kModeAp ? "Access point" : mode == kModeBoth ? "Both" : "Client";
}

// --- typed fields: their keyboard and their rule

enum class Field : uint8_t {
    kApName,
    kApPassword,
    kHostname,
    kHiddenSsid,
    kHiddenPassword,
    kAddress,
    kNetmask,
    kGateway,
    kDns,
};

inline constexpr size_t kIpSize = 16;  // "255.255.255.255" + NUL

struct FieldSpec {
    const char *title;
    const char *placeholder;
    bool masked;
    bool numeric;  // the keyboard's digits
    uint8_t max;   // characters
    const char *hint;
};

constexpr FieldSpec Spec(Field f) {
    switch (f) {
        case Field::kApName:
            return {"Access point name", "name", false, false, 32, "1..32 characters."};
        case Field::kApPassword:
            return {"Access point password", "password", true, false, 63,
                    "8..63 characters, or empty: an open access point."};
        case Field::kHostname:
            return {"Hostname", "name", false, false, 32,
                    "The name on the network: 1..32 of a-z, 0-9, '-'."};
        case Field::kHiddenSsid:
            return {"Other network", "network name", false, false, 32,
                    "The network's name (SSID), as its router has it."};
        case Field::kHiddenPassword:
            return {"Other network", "password", true, false, 64,
                    "8..63 characters, or empty: an open network."};
        case Field::kAddress:
            return {"IP address", "", false, true, 15, "This device's address, e.g. 192.168.1.50."};
        case Field::kNetmask:
            return {"Netmask", "", false, true, 15, "Usually 255.255.255.0."};
        case Field::kGateway:
            return {"Gateway", "", false, true, 15, "The router's address, e.g. 192.168.1.1."};
        case Field::kDns:
            return {"DNS", "", false, true, 15, "A name server, e.g. 192.168.1.1, or empty."};
    }
    return {"", "", false, false, 0, ""};
}

constexpr size_t Length(const char *s) {
    size_t n = 0;
    for (; s[n] != '\0'; ++n) {
    }
    return n;
}

// config::ValidHostname's rule.
constexpr bool HostnameOk(const char *s) {
    const size_t n = Length(s);
    if (n == 0 || n > 32 || s[0] == '-' || s[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

// config::ParseIpv4's rule: four decimal octets 0..255, no leading zeros.
constexpr bool Ipv4Ok(const char *s) {
    int parts = 0;
    while (true) {
        int value = 0;
        int digits = 0;
        const char *start = s;
        for (; *s >= '0' && *s <= '9'; ++s, ++digits) {
            value = value * 10 + (*s - '0');
            if (digits >= 3) {
                return false;
            }
        }
        if (digits == 0 || value > 255 || (digits > 1 && *start == '0')) {
            return false;
        }
        ++parts;
        if (*s == '\0') {
            return parts == 4;
        }
        if (*s != '.' || parts == 4) {
            return false;
        }
        ++s;
    }
}

// What is wrong with `value` for `f`, nullptr when it fits.
constexpr const char *Check(Field f, const char *value) {
    const size_t n = Length(value);
    switch (f) {
        case Field::kApName:
        case Field::kHiddenSsid:
            return n >= 1 && n <= 32 ? nullptr : "1..32 characters";
        case Field::kApPassword:
            return n == 0 || (n >= 8 && n <= 63) ? nullptr : "8..63 characters, or empty";
        case Field::kHiddenPassword:
            return n == 0 || PasswordOk(value) ? nullptr : "8..63 characters, or empty";
        case Field::kHostname:
            return HostnameOk(value) ? nullptr : "1..32 of a-z, 0-9, '-'";
        case Field::kDns:
            return n == 0 || Ipv4Ok(value) ? nullptr : "an address like 192.168.1.1";
        case Field::kAddress:
        case Field::kNetmask:
        case Field::kGateway:
            return Ipv4Ok(value) ? nullptr : "an address like 192.168.1.1";
    }
    return nullptr;
}

// A saved network's address: DHCP, or static (edited whole, saved whole).
struct StaticIp {
    bool enabled;
    char address[kIpSize];
    char netmask[kIpSize];
    char gateway[kIpSize];
    char dns[kIpSize];
};

constexpr char *StaticField(StaticIp &ip, Field f) {
    return f == Field::kAddress   ? ip.address
           : f == Field::kNetmask ? ip.netmask
           : f == Field::kGateway ? ip.gateway
           : f == Field::kDns     ? ip.dns
                                  : nullptr;
}

// Static: the address, the netmask and the gateway are needed.
constexpr const char *CheckStatic(const StaticIp &ip) {
    if (!ip.enabled) {
        return nullptr;
    }
    if (!Ipv4Ok(ip.address)) {
        return "the IP address is missing";
    }
    if (!Ipv4Ok(ip.netmask)) {
        return "the netmask is missing";
    }
    if (!Ipv4Ok(ip.gateway)) {
        return "the gateway is missing";
    }
    return ip.dns[0] == '\0' || Ipv4Ok(ip.dns) ? nullptr : "the DNS is not an address";
}

}  // namespace ui::wifi_view
