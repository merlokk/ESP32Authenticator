#include "zones.h"

namespace tz {

namespace {

// Groups repeat their rules on purpose: the EU switches together, and a shared
// constant would hide the day one country leaves.
constexpr Zone kZones[] = {
    {"UTC", "UTC0"},

    // The EU's zones by their own names (same rules as the cities below).
    {"WET", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"CET", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"EET", "EET-2EEST,M3.5.0/3,M10.5.0/4"},

    // Western Europe
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},

    // Central Europe
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},

    // Eastern Europe: the EU switches at 01:00 UTC = 03:00 / 04:00 local.
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Chisinau", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Riga", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Tallinn", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Vilnius", "EET-2EEST,M3.5.0/3,M10.5.0/4"},

    // No daylight saving
    {"Europe/Istanbul", "<+03>-3"},
    {"Europe/Minsk", "<+03>-3"},
    {"Europe/Moscow", "MSK-3"},

    // Asia
    {"Asia/Almaty", "<+05>-5"},
    {"Asia/Baku", "<+04>-4"},
    {"Asia/Bangkok", "<+07>-7"},
    {"Asia/Dubai", "<+04>-4"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Nicosia", "EET-2EEST,M3.5.0/3,M10.5.0/4"},  // EU dates
    {"Asia/Seoul", "KST-9"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Singapore", "<+08>-8"},
    {"Asia/Tashkent", "<+05>-5"},
    {"Asia/Tbilisi", "<+04>-4"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Yerevan", "<+04>-4"},

    // Africa
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Lagos", "WAT-1"},
    {"Africa/Nairobi", "EAT-3"},

    // Americas: US/Canada switch on the 2nd Sunday of March, 1st of November.
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Bogota", "<-05>5"},
    {"America/Buenos_Aires", "<-03>3"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Lima", "<-05>5"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "CST6"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Sao_Paulo", "<-03>3"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu", "HST10"},

    // Southern hemisphere: summer time in the other half of the year.
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane", "AEST-10"},
    {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Perth", "AWST-8"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

// Renamed zones and what people type; kept apart so the listing has one name
// per zone.
struct Alias {
    const char *from;
    const char *to;
};

constexpr Alias kAliases[] = {
    {"Europe/Kiev", "Europe/Kyiv"},
    {"Europe/Nicosia", "Asia/Nicosia"},
    {"Asia/Calcutta", "Asia/Kolkata"},
    {"Etc/UTC", "UTC"},
    {"GMT", "UTC"},
    {"America/Argentina/Buenos_Aires", "America/Buenos_Aires"},
};

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsIgnoringCase(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        if (Lower(*a) != Lower(*b)) {
            return false;
        }
    }
    return *a == *b;
}

bool Equals(const char *a, const char *b) {
    for (; *a != '\0' && *a == *b; ++a, ++b) {
    }
    return *a == *b;
}

}  // namespace

size_t Count() { return sizeof(kZones) / sizeof(kZones[0]); }

const Zone &At(size_t index) { return kZones[index < Count() ? index : 0]; }

const char *Lookup(const char *name) {
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    for (const Alias &alias : kAliases) {
        if (EqualsIgnoringCase(name, alias.from)) {
            name = alias.to;
            break;
        }
    }
    for (const Zone &zone : kZones) {
        if (EqualsIgnoringCase(name, zone.name)) {
            return zone.posix;
        }
    }
    return nullptr;
}

const char *NameFor(const char *posix) {
    if (posix == nullptr) {
        return nullptr;
    }
    // First match: WET/CET/EET head the table, so a shared rule is named after
    // the zone family rather than the first city.
    for (const Zone &zone : kZones) {
        if (Equals(posix, zone.posix)) {
            return zone.name;
        }
    }
    return nullptr;
}

bool LooksLikePosix(const char *value) {
    if (value == nullptr) {
        return false;
    }
    // In a rule every '/' follows a digit (M3.5.0/3); in a name it separates
    // letters (Europe/Kyiv).
    size_t length = 0;
    bool digit = false;
    for (const char *p = value; *p != '\0'; ++p, ++length) {
        if (*p == '/' && (p == value || p[-1] < '0' || p[-1] > '9')) {
            return false;
        }
        if (*p == ' ') {
            return false;
        }
        digit = digit || (*p >= '0' && *p <= '9');
    }
    // An abbreviation and an offset: "UTC0" is the shortest real one.
    return length >= 4 && length < kMaxPosixLength && digit;
}

const char *Resolve(const char *zone) {
    const char *posix = Lookup(zone);
    if (posix != nullptr) {
        return posix;
    }
    return LooksLikePosix(zone) ? zone : nullptr;
}

}  // namespace tz
