#pragma once

// The status bar (x4-design.md) at the top of every screen: Wi-Fi, Bluetooth,
// the microSD and the server's sync on the left, the time and the battery on the right. Pure
// (host-tested): what the board reports in, the two texts out; the ui task
// sets a label only when its text changed (each change is an e-ink refresh).

#include <cstddef>
#include <cstdint>

namespace ui::status {

// LVGL's built-in symbols (lv_symbol_def.h; the Montserrat fonts carry them).
inline constexpr const char *kWifi = "\xEF\x87\xAB";
inline constexpr const char *kBluetooth = "\xEF\x8A\x93";
inline constexpr const char *kSdCard = "\xEF\x9F\x82";
inline constexpr const char *kCharge = "\xEF\x83\xA7";
inline constexpr const char *kSync = "\xEF\x80\xA1";    // LV_SYMBOL_REFRESH
inline constexpr const char *kUpload = "\xEF\x82\x93";  // LV_SYMBOL_UPLOAD
// Empty, 1, 2, 3, full.
inline constexpr const char *kBattery[5] = {"\xEF\x89\x84", "\xEF\x89\x83", "\xEF\x89\x82",
                                            "\xEF\x89\x81", "\xEF\x89\x80"};

enum class Wifi : uint8_t {
    kOff,
    kTrying,      // connecting, or waiting for the next attempt
    kOnline,      // joined, the internet answers (or not checked yet)
    kNoInternet,  // joined, the internet check fails
    kAp,          // our access point (asked for, or the setup window)
};

enum class Ble : uint8_t {
    kOff,
    kWaiting,    // advertising, no host
    kConnected,
    kPairing,    // the pairing window is open
};

// The stores' sync with the server (server::sync::Icon).
enum class Sync : uint8_t {
    kHidden,    // no server
    kOff,       // not registered
    kPending,   // changes to send or to take
    kOk,
    kConflict,  // waits for a person: Settings > Server > Sync
};

struct Status {
    Wifi wifi = Wifi::kOff;
    Ble ble = Ble::kOff;
    int8_t battery = -1;  // percent, -1 no gauge
    bool charging = false;
    bool sd = false;         // mounted
    Sync sync = Sync::kHidden;
    bool fido_wait = false;  // a FIDO2 request waits for the user
    bool time_set = false;  // the clock is believable
    uint8_t hour = 0;       // local
    uint8_t minute = 0;
};

inline constexpr size_t kTextSize = 64;

// The battery icon for a level: a bar per 25 %, rounded.
constexpr uint8_t BatteryBars(int8_t percent) {
    return percent <= 12 ? 0 : percent <= 37 ? 1 : percent <= 62 ? 2 : percent <= 87 ? 3 : 4;
}

// The gauge wanders by a percent: the shown level moves only by 2 or more,
// or to 100 / 0, so the bar is not redrawn for noise.
constexpr int8_t ShownLevel(int8_t shown, int8_t now) {
    if (shown < 0 || now < 0 || now == 100 || now == 0) {
        return now;
    }
    const int diff = now > shown ? now - shown : shown - now;
    return diff >= 2 ? now : shown;
}

constexpr const char *WifiText(Wifi w) {
    return w == Wifi::kTrying ? "..." : w == Wifi::kNoInternet ? "!" : w == Wifi::kAp ? "AP" : "";
}

constexpr const char *BleText(Ble b) {
    return b == Ble::kWaiting ? "..." : b == Ble::kPairing ? "pair" : "";
}

constexpr const char *SyncText(Sync s) {
    return s == Sync::kOff ? "off" : s == Sync::kPending ? kUpload : s == Sync::kConflict ? "!" : "";
}

// Text into a fixed buffer, cut at its end, always terminated.
class Text {
   public:
    constexpr Text(char *out, size_t size) : out_(out), size_(size) { out_[0] = '\0'; }
    constexpr Text &Add(const char *s) {
        for (; *s != '\0' && n_ + 1 < size_; ++s) {
            out_[n_++] = *s;
        }
        out_[n_] = '\0';
        return *this;
    }
    constexpr Text &Number(unsigned v, uint8_t min_digits = 1) {
        char d[10] = {};
        uint8_t k = 0;
        do {
            d[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0 && k < sizeof d);
        while (k < min_digits && k < sizeof d) {
            d[k++] = '0';
        }
        while (k > 0 && n_ + 1 < size_) {
            out_[n_++] = d[--k];
        }
        out_[n_] = '\0';
        return *this;
    }
    constexpr size_t size() const { return n_; }

   private:
    char *out_;
    size_t size_;
    size_t n_ = 0;
};

inline constexpr const char *kGap = "   ";

// Left: "<wifi> [state]   <bt> [state]   <sd>   <sync> [state]"; what is off
// is left out.
// (While FIDO2 waits, its prompt window says what the buttons do.)
constexpr void Left(const Status &s, char *out, size_t size) {
    Text t(out, size);
    auto add = [&t](const char *icon, const char *state) {
        if (t.size() > 0) {
            t.Add(kGap);
        }
        t.Add(icon);
        if (state[0] != '\0') {
            t.Add(" ").Add(state);
        }
    };
    if (s.wifi != Wifi::kOff) {
        add(kWifi, WifiText(s.wifi));
    }
    if (s.ble != Ble::kOff) {
        add(kBluetooth, BleText(s.ble));
    }
    if (s.sd) {
        add(kSdCard, "");
    }
    if (s.sync != Sync::kHidden) {
        add(kSync, SyncText(s.sync));
    }
}

// Right: "12:34   <charge><battery> 75%"; no time before the clock is set,
// no battery without a gauge.
constexpr void Right(const Status &s, char *out, size_t size) {
    Text t(out, size);
    if (s.time_set) {
        t.Number(s.hour % 24u, 2).Add(":").Number(s.minute % 60u, 2);
    }
    if (s.battery >= 0) {
        if (t.size() > 0) {
            t.Add(kGap);
        }
        if (s.charging) {
            t.Add(kCharge);
        }
        t.Add(kBattery[BatteryBars(s.battery)]).Add(" ").Number(static_cast<unsigned>(s.battery)).Add("%");
    }
}

}  // namespace ui::status
