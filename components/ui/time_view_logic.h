#pragma once

// The Date & time window of Settings (x4-design.md): the clock big, then the
// time zone (a region, then a city, in pages), how often to sync, the server
// and the last sync, the time set by hand (- / + per field) and Sync now.
// The device fills View and the zone table and carries out the changes
// (ui::TimeHooks). Pure (host-tested).

#include <cstddef>
#include <cstdint>

#include "status_logic.h"

namespace ui::time_view {

// --- the zones: a region (the name's first part), then a city

inline constexpr const char *kRegions[] = {"Europe",    "Asia",    "America", "Africa",
                                           "Australia", "Pacific", "Other"};
inline constexpr uint8_t kRegionCount = sizeof kRegions / sizeof kRegions[0];
inline constexpr uint8_t kOther = kRegionCount - 1;  // UTC, CET, ...
inline constexpr uint8_t kZoneRows = 9;              // a page of cities

// The region a zone's name is in: kOther without a known first part.
constexpr uint8_t RegionOf(const char *name) {
    for (uint8_t r = 0; r < kOther; ++r) {
        const char *a = kRegions[r];
        const char *b = name;
        for (; *a != '\0' && *a == *b; ++a, ++b) {
        }
        if (*a == '\0' && *b == '/') {
            return r;
        }
    }
    return kOther;
}

// The city to show: after the last '/', '_' read as a space.
constexpr void City(const char *name, char *out, size_t size) {
    const char *last = name;
    for (const char *p = name; *p != '\0'; ++p) {
        if (*p == '/') {
            last = p + 1;
        }
    }
    size_t n = 0;
    for (; *last != '\0' && n + 1 < size; ++last) {
        out[n++] = *last == '_' ? ' ' : *last;
    }
    out[n] = '\0';
}

// A zone's city before another's, A to Z (the region's list).
constexpr bool CityLess(const char *a, const char *b) {
    const char *x = a;
    const char *y = b;
    for (const char *p = a; *p != '\0'; ++p) {
        if (*p == '/') {
            x = p + 1;
        }
    }
    for (const char *p = b; *p != '\0'; ++p) {
        if (*p == '/') {
            y = p + 1;
        }
    }
    for (; *x != '\0' && *x == *y; ++x, ++y) {
    }
    return static_cast<unsigned char>(*x) < static_cast<unsigned char>(*y);
}

// --- how often to sync (time.syncHours)

struct Option {
    uint8_t value;
    const char *label;
};

inline constexpr Option kSync[] = {{0, "Never"},          {1, "Every hour"},
                                   {6, "Every 6 hours"},  {12, "Every 12 hours"},
                                   {24, "Every day"},     {168, "Every week"}};
inline constexpr uint8_t kSyncCount = sizeof kSync / sizeof kSync[0];

constexpr int SyncIndex(uint8_t hours) {
    for (uint8_t i = 0; i < kSyncCount; ++i) {
        if (kSync[i].value == hours) {
            return i;
        }
    }
    return -1;
}

constexpr void SyncLabel(uint8_t hours, char *out, size_t size) {
    status::Text t(out, size);
    const int i = SyncIndex(hours);
    if (i >= 0) {
        t.Add(kSync[i].label);
    } else {
        t.Add("Every ").Number(hours).Add(" hours");
    }
}

// --- the time by hand

enum Field : uint8_t { kYear, kMonth, kDay, kHour, kMinute, kFields };

inline constexpr uint16_t kFirstYear = 2024;  // rtc::Believable's range
inline constexpr uint16_t kLastYear = 2099;

constexpr bool Leap(uint16_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

constexpr uint8_t DaysIn(uint16_t year, uint8_t month) {
    constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && Leap(year) ? 29 : kDays[(month - 1) % 12];
}

struct Manual {
    uint16_t value[kFields];  // year, month 1..12, day, hour 0..23, minute
};

constexpr uint16_t Low(const Manual &, Field f) {
    return f == kYear ? kFirstYear : f == kMonth || f == kDay ? 1 : 0;
}

constexpr uint16_t High(const Manual &m, Field f) {
    return f == kYear    ? kLastYear
           : f == kMonth ? 12
           : f == kDay   ? DaysIn(m.value[kYear], static_cast<uint8_t>(m.value[kMonth]))
           : f == kHour  ? 23
                         : 59;
}

// A field one up or down, round within its range; the day kept inside its
// month when the month or the year moves.
constexpr void Step(Manual &m, Field f, int delta) {
    const int low = Low(m, f);
    const int span = High(m, f) - low + 1;
    const int v = (m.value[f] - low + delta % span + span) % span + low;
    m.value[f] = static_cast<uint16_t>(v);
    const uint16_t last = DaysIn(m.value[kYear], static_cast<uint8_t>(m.value[kMonth]));
    if (m.value[kDay] > last) {
        m.value[kDay] = last;
    }
}

constexpr const char *FieldName(Field f) {
    return f == kYear ? "Year" : f == kMonth ? "Month" : f == kDay ? "Day" : f == kHour ? "Hour" : "Minute";
}

// --- what the window shows

struct View {
    bool time_set;     // the clock is believable
    char clock[8];     // "16:12"
    char date[40];     // "Saturday, 24 October 2026"
    char zone[41];     // config time.zone
    char offset[24];   // "UTC+03:00, summer"
    char server[65];
    char last_sync[24];  // "5 min ago", "never", "now"
    uint8_t sync_hours;
    uint16_t now[kFields];  // the local time now, for the hand page
};

constexpr bool Same(const char *a, const char *b) {
    for (; *a != '\0' && *a == *b; ++a, ++b) {
    }
    return *a == *b;
}

// The hand page's numbers aside: they move every minute with the clock.
constexpr bool Equal(const View &a, const View &b) {
    return a.time_set == b.time_set && Same(a.clock, b.clock) && Same(a.date, b.date) &&
           Same(a.zone, b.zone) && Same(a.offset, b.offset) && Same(a.server, b.server) &&
           Same(a.last_sync, b.last_sync) && a.sync_hours == b.sync_hours;
}

}  // namespace ui::time_view
