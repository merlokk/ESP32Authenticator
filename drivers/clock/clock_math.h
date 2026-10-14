#pragma once

// Clock arithmetic with no hardware: civil date <-> Unix time (UTC), the
// believable range, and the timer clock's saved state. Host-tested.

#include <cstdint>

namespace rtc {

// UTC, never local time: a zone is applied when a time is shown.
struct DateTime {
    uint16_t year;   // full year
    uint8_t month;   // 1..12
    uint8_t day;     // 1..31
    uint8_t hour;    // 0..23
    uint8_t minute;  // 0..59
    uint8_t second;  // 0..59
};

// An answer outside these years is a clock that was never set (1970) or a bad
// server, not the time: an unset clock beats a plausible wrong one.
inline constexpr int kEarliestYear = 2024;
inline constexpr int kLatestYear = 2099;

constexpr bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

constexpr int DaysInMonth(int year, int month) {
    constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && IsLeapYear(year) ? 29 : kDays[month - 1];
}

constexpr bool Valid(const DateTime &t) {
    return t.year >= 1970 && t.year <= 9999 && t.month >= 1 && t.month <= 12 && t.day >= 1 &&
           t.day <= DaysInMonth(t.year, t.month) && t.hour <= 23 && t.minute <= 59 &&
           t.second <= 59;
}

// Days since 1970-01-01 (H. Hinnant's days_from_civil).
constexpr int64_t DaysFromCivil(int year, int month, int day) {
    const int y = year - (month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + doe - 719468;
}

// Seconds since the epoch. `t` must be Valid().
constexpr int64_t ToUnix(const DateTime &t) {
    return DaysFromCivil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 +
           t.second;
}

// The reverse (civil_from_days). For seconds >= 0.
constexpr DateTime FromUnix(int64_t seconds) {
    const int64_t days = seconds / 86400;
    const int64_t rest = seconds % 86400;
    const int64_t z = days + 719468;
    const int64_t era = z / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    DateTime t = {};
    t.year = static_cast<uint16_t>(year);
    t.month = static_cast<uint8_t>(month);
    t.day = static_cast<uint8_t>(day);
    t.hour = static_cast<uint8_t>(rest / 3600);
    t.minute = static_cast<uint8_t>(rest / 60 % 60);
    t.second = static_cast<uint8_t>(rest % 60);
    return t;
}

constexpr bool Believable(int64_t utc_seconds) {
    return utc_seconds >= DaysFromCivil(kEarliestYear, 1, 1) * 86400 &&
           utc_seconds < DaysFromCivil(kLatestYear + 1, 1, 1) * 86400;
}

// --- Timer clock --------------------------------------------------------------
// UTC kept as "the time when set" + "the timer when set". The timer (the RTC
// timer on the chip) runs across software resets and restarts from zero at
// power-on, so the state lives in memory a software reset keeps: garbage after
// a power cut, caught by the magic and the check.

struct TimerState {
    uint32_t magic;
    uint32_t check;
    int64_t utc_us_at_set;
    uint64_t timer_us_at_set;
};

inline constexpr uint32_t kTimerMagic = 0x434C4B31;  // "CLK1"

constexpr uint32_t TimerCheck(const TimerState &s) {
    // FNV-1a over the two values: random memory passes with p = 2^-32.
    uint32_t h = 2166136261u ^ s.magic;
    const uint64_t words[] = {static_cast<uint64_t>(s.utc_us_at_set), s.timer_us_at_set};
    for (uint64_t w : words) {
        for (int i = 0; i < 8; ++i) {
            h = (h ^ static_cast<uint8_t>(w >> (8 * i))) * 16777619u;
        }
    }
    return h;
}

constexpr TimerState TimerSet(int64_t utc_us, uint64_t timer_us) {
    TimerState s = {kTimerMagic, 0, utc_us, timer_us};
    s.check = TimerCheck(s);
    return s;
}

// False when the state is not ours (power cut) or the timer went backwards
// (it restarted: the saved offset no longer means anything).
constexpr bool TimerRead(const TimerState &s, uint64_t timer_us, int64_t *utc_us) {
    if (s.magic != kTimerMagic || s.check != TimerCheck(s) || timer_us < s.timer_us_at_set) {
        return false;
    }
    *utc_us = s.utc_us_at_set + static_cast<int64_t>(timer_us - s.timer_us_at_set);
    return true;
}

}  // namespace rtc
