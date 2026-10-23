#pragma once

// The pure half of the BM8563 RTC (PCF8563-compatible, I2C 0x51; host-tested):
// registers, BCD, the time counters <-> DateTime (UTC), and when to start the
// chip so its second ticks with the system clock's. Register map: NXP PCF8563
// datasheet (Table 4, 8.4 "STOP bit function"); the start delay is measured.

#include <cstddef>
#include <cstdint>

#include "clock_math.h"

namespace rtc::bm8563 {

inline constexpr uint8_t kAddress = 0x51;

inline constexpr uint8_t kRegControl1 = 0x00;  // bit 5 STOP
inline constexpr uint8_t kRegSeconds = 0x02;   // bit 7 VL; then minutes .. years
inline constexpr size_t kCounterCount = 7;     // seconds minutes hours days weekdays months years

inline constexpr uint8_t kControl1Stop = 1 << 5;
// Voltage low: the oscillator stopped or never ran since power-up; the time is
// not to be believed until it is written again.
inline constexpr uint8_t kSecondsVl = 1 << 7;
// Months bit 7: the century flag. Ignored on read (the believable range is
// 2024..2099), written 0 (20xx).
inline constexpr uint8_t kMonthsCentury = 1 << 7;

// After STOP is cleared the first second increments this much later. Measured
// on the X4 Pro's BM8563: 1.000 s (the whole prescaler restarts); the NXP
// PCF8563 says 0.507813 s, and with that the clock ran 492 ms late.
inline constexpr int64_t kFirstTickUs = 1000000;

constexpr uint8_t FromBcd(uint8_t value) {
    return static_cast<uint8_t>((value >> 4) * 10 + (value & 0x0F));
}

constexpr uint8_t ToBcd(uint8_t value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

constexpr bool IsBcd(uint8_t value) { return (value & 0x0F) <= 9 && (value >> 4) <= 9; }

enum class Decoded : uint8_t {
    kOk,
    kVoltageLow,  // VL set: no time
    kBadFields,   // not BCD, not a date, or outside the believable range
};

// The seven counters from 0x02, read in one burst (a read freezes them).
constexpr Decoded Decode(const uint8_t (&raw)[kCounterCount], DateTime *out) {
    const uint8_t fields[] = {
        static_cast<uint8_t>(raw[0] & 0x7F), static_cast<uint8_t>(raw[1] & 0x7F),
        static_cast<uint8_t>(raw[2] & 0x3F), static_cast<uint8_t>(raw[3] & 0x3F),
        static_cast<uint8_t>(raw[5] & 0x1F), raw[6]};
    for (const uint8_t f : fields) {
        if (!IsBcd(f)) {
            return Decoded::kBadFields;
        }
    }
    DateTime t = {};
    t.second = FromBcd(fields[0]);
    t.minute = FromBcd(fields[1]);
    t.hour = FromBcd(fields[2]);
    t.day = FromBcd(fields[3]);
    t.month = FromBcd(fields[4]);
    t.year = static_cast<uint16_t>(2000 + FromBcd(fields[5]));
    *out = t;
    if ((raw[0] & kSecondsVl) != 0) {
        return Decoded::kVoltageLow;
    }
    if (!Valid(t) || !Believable(ToUnix(t))) {
        return Decoded::kBadFields;
    }
    return Decoded::kOk;
}

// 0 = Sunday, as the chip counts (1970-01-01 was a Thursday).
constexpr uint8_t Weekday(int64_t utc_seconds) {
    return static_cast<uint8_t>((utc_seconds / 86400 + 4) % 7);
}

// The counters for `utc_seconds` (believable). VL and the century bit 0.
constexpr void Encode(int64_t utc_seconds, uint8_t (&raw)[kCounterCount]) {
    const DateTime t = FromUnix(utc_seconds);
    raw[0] = ToBcd(t.second);
    raw[1] = ToBcd(t.minute);
    raw[2] = ToBcd(t.hour);
    raw[3] = ToBcd(t.day);
    raw[4] = Weekday(utc_seconds);
    raw[5] = ToBcd(t.month);
    raw[6] = ToBcd(static_cast<uint8_t>(t.year - 2000));
}

// Setting the chip to `utc_us`, second-accurate: write the counters for
// `seconds` with STOP set, clear STOP at `start_at_us` (the same time base),
// and the chip ticks to seconds + 1 when that second begins. At least
// `lead_us` from now, for the writes.
struct WritePlan {
    int64_t seconds;
    int64_t start_at_us;
};

constexpr WritePlan PlanWrite(int64_t utc_us, int64_t lead_us) {
    int64_t seconds = utc_us / 1000000;
    while ((seconds + 1) * 1000000 - kFirstTickUs < utc_us + lead_us) {
        ++seconds;
    }
    return {seconds, (seconds + 1) * 1000000 - kFirstTickUs};
}

constexpr const char *Name(Decoded d) {
    switch (d) {
        case Decoded::kOk: return "ok";
        case Decoded::kVoltageLow: return "voltage low (time lost)";
        case Decoded::kBadFields: return "not a believable time";
    }
    return "?";
}

}  // namespace rtc::bm8563
