#pragma once

// The Display & power window of Settings (x4-design.md): the battery, then a
// row per setting with its value; a tap lists the choices (a check at the
// one in use), a tap on one keeps it. The device fills View and keeps a
// choice (ui::DisplayHooks). Pure (host-tested).

#include <cstddef>
#include <cstdint>

#include "status_logic.h"

namespace ui::display_view {

enum class Setting : uint8_t {
    kIdle,       // power.idleMinutes: off after this long idle, 0 never
    kIdleUsb,    // power.idleMinutesUsb: the same on USB power, 0 never
    kKeepPin,    // power.keepPinHours: no PIN after a sleep this short, 0 always
    kWallpaper,  // display.wallpaper: 0 grey, 1 black & white
    kHoldRail,   // power.holdRail: 1 the peripherals' power kept through a sleep
    kLightOff,     // frontlight.offMinutes: the light off after this long idle, 0 never
    kLightOffUsb,  // frontlight.offMinutesUsb: the same on USB power
};

inline constexpr Setting kSettings[] = {Setting::kLightOff, Setting::kLightOffUsb, Setting::kIdle,
                                        Setting::kIdleUsb,  Setting::kKeepPin,     Setting::kWallpaper,
                                        Setting::kHoldRail};
inline constexpr uint8_t kSettingCount = sizeof kSettings / sizeof kSettings[0];

struct Option {
    uint16_t value;
    const char *label;
};

inline constexpr Option kIdle[] = {{0, "Never"},   {2, "2 min"},   {5, "5 min"},  {10, "10 min"},
                                   {15, "15 min"}, {30, "30 min"}, {60, "1 hour"}};
inline constexpr Option kKeepPin[] = {{0, "Always"},    {1, "After 1 hour"},  {4, "After 4 hours"},
                                      {8, "After 8 hours"}, {24, "After a day"}, {72, "After 3 days"},
                                      {168, "After a week"}};
inline constexpr Option kWallpaper[] = {{0, "Grey"}, {1, "Black & white"}};
inline constexpr Option kLight[] = {{0, "Never"},  {1, "1 min"},   {2, "2 min"},
                                    {5, "5 min"},  {10, "10 min"}, {30, "30 min"}};
// Off: no drain while asleep, but the PIN kept through a sleep is lost.
inline constexpr Option kHoldRail[] = {{1, "Kept"}, {0, "Off"}};

constexpr const Option *Options(Setting s, size_t *count) {
    switch (s) {
        case Setting::kIdle:
        case Setting::kIdleUsb: *count = sizeof kIdle / sizeof kIdle[0]; return kIdle;
        case Setting::kKeepPin: *count = sizeof kKeepPin / sizeof kKeepPin[0]; return kKeepPin;
        case Setting::kWallpaper: *count = sizeof kWallpaper / sizeof kWallpaper[0]; return kWallpaper;
        case Setting::kHoldRail: *count = sizeof kHoldRail / sizeof kHoldRail[0]; return kHoldRail;
        case Setting::kLightOff:
        case Setting::kLightOffUsb: *count = sizeof kLight / sizeof kLight[0]; return kLight;
    }
    *count = 0;
    return nullptr;
}

constexpr const char *Title(Setting s) {
    return s == Setting::kIdle       ? "Power off after"
           : s == Setting::kIdleUsb  ? "On USB, off after"
           : s == Setting::kKeepPin  ? "PIN after sleep"
           : s == Setting::kWallpaper ? "Wallpaper"
           : s == Setting::kLightOff  ? "Light off after"
           : s == Setting::kLightOffUsb ? "On USB, light off after"
                                     : "Power in sleep";
}

// The choice in use; -1 when the value is none of them (set on the site).
constexpr int Index(Setting s, uint16_t value) {
    size_t n = 0;
    const Option *o = Options(s, &n);
    for (size_t i = 0; i < n; ++i) {
        if (o[i].value == value) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The value on its row: the choice's label, or "N min" / "after N h".
constexpr void Label(Setting s, uint16_t value, char *out, size_t size) {
    status::Text t(out, size);
    size_t n = 0;
    const Option *o = Options(s, &n);
    const int i = Index(s, value);
    if (i >= 0) {
        t.Add(o[i].label);
    } else if (s == Setting::kIdle || s == Setting::kIdleUsb || s == Setting::kLightOff ||
               s == Setting::kLightOffUsb) {
        t.Number(value).Add(" min");
    } else if (s == Setting::kKeepPin) {
        t.Add("After ").Number(value).Add(" hours");
    } else {
        t.Add("?");
    }
}

// What the window shows; drawn again when it changes.
struct View {
    int8_t battery;  // percent, -1 no gauge
    bool charging;
    uint16_t value[kSettingCount];  // by kSettings
};

constexpr bool Equal(const View &a, const View &b) {
    if (a.battery != b.battery || a.charging != b.charging) {
        return false;
    }
    for (uint8_t i = 0; i < kSettingCount; ++i) {
        if (a.value[i] != b.value[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace ui::display_view
