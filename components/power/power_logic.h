#pragma once

// The pure half of power-off (host-tested): which pins are latched (gpio
// hold) through deep sleep, and at what level. As FreeInk's PowerManager
// (powerDownRailsForSleep) on the X4 Pro: SD and touch power off (their
// enables are active-low: held high), the panel's RST held high so the
// controller stays in its deep sleep; the frontlight's PWM pins held low
// (isolated, they would float). Unlike FreeInk, the peripheral rail (GPIO1)
// held high (`hold_rail`, config power.holdRail): on the battery the board's
// power drops without it, a cold boot that loses RTC memory (measured). Boot releases the same pins (driven to
// the same levels first) before any driver touches them. Wake: the Power
// button (RTC ext1, low).
//
// Also: when idling turns it off (IdleOff), and the record that keeps a
// secret (the vault key) through one deep sleep (Kept).

#include <cstddef>
#include <cstdint>

#include "board_spec.h"

namespace power {

struct Hold {
    int8_t pin;
    uint8_t level;
};

inline constexpr size_t kMaxHolds = 6;

struct Holds {
    Hold items[kMaxHolds] = {};
    size_t size = 0;

    constexpr void Add(int8_t pin, uint8_t level) {
        if (pin != boards::kNoPin && size < kMaxHolds) {
            items[size++] = {pin, level};
        }
    }
};

constexpr Holds SleepHolds(const boards::Board &b, bool hold_rail = true) {
    Holds out;
    if (hold_rail) {
        out.Add(b.power_pins.peripheral_rail, 1);
    }
    out.Add(b.display_pins.reset, 1);
    out.Add(b.sd_pins.power_enable, 1);
    out.Add(b.touch_pins.power_enable, 1);
    out.Add(b.frontlight_pins.cool, 0);
    out.Add(b.frontlight_pins.warm, 0);
    return out;
}

// Off after `idle_minutes` with nothing done (0: never); `held`: something
// keeps it on (a USB host, the pairing window).
constexpr bool IdleOff(uint64_t now_ms, uint64_t last_activity_ms, uint16_t idle_minutes,
                       bool held) {
    return idle_minutes != 0 && !held && now_ms >= last_activity_ms &&
           now_ms - last_activity_ms >= static_cast<uint64_t>(idle_minutes) * 60000;
}

// A secret kept through one deep sleep, in RTC memory (power.cpp: survives
// deep sleep and resets, not a power loss): good from the sleep for `hours`
// on the battery-backed clock (UTC seconds; 0: not set). Taken once at the
// wake, wiped either way.
inline constexpr size_t kKeptBytes = 64;
inline constexpr uint32_t kKeptMagic = 0x5450454B;  // "KEPT"
// The wake may read the clock a little behind the sleep (whole seconds, a sync).
inline constexpr int64_t kKeptSlackSeconds = 120;

struct Kept {
    uint32_t magic;
    uint32_t check;  // FNV-1a over the rest: RTC memory is noise after a power loss
    int64_t asleep_utc;
    int64_t until_utc;
    uint32_t size;
    uint8_t data[kKeptBytes];
};

constexpr uint32_t KeptCheck(const Kept &k) {
    uint32_t h = 2166136261u;
    const auto mix = [&h](uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            h = (h ^ static_cast<uint8_t>(v >> (8 * i))) * 16777619u;
        }
    };
    mix(static_cast<uint64_t>(k.asleep_utc), 8);
    mix(static_cast<uint64_t>(k.until_utc), 8);
    mix(k.size, 4);
    for (uint8_t b : k.data) {
        mix(b, 1);
    }
    return h;
}

// False (nothing to keep): no hours, no clock, too big.
constexpr bool FillKept(Kept *k, const uint8_t *data, size_t size, int64_t now_utc,
                        uint32_t hours) {
    if (hours == 0 || now_utc <= 0 || size > kKeptBytes) {
        return false;
    }
    *k = Kept{};
    k->asleep_utc = now_utc;
    k->until_utc = now_utc + static_cast<int64_t>(hours) * 3600;
    k->size = static_cast<uint32_t>(size);
    for (size_t i = 0; i < size; ++i) {
        k->data[i] = data[i];
    }
    k->check = KeptCheck(*k);
    k->magic = kKeptMagic;
    return true;
}

// `now_utc` 0: the clock is not believable (the chip lost it): not usable.
constexpr bool KeptUsable(const Kept &k, int64_t now_utc) {
    return k.magic == kKeptMagic && k.check == KeptCheck(k) && k.size <= kKeptBytes &&
           now_utc > 0 && now_utc + kKeptSlackSeconds >= k.asleep_utc && now_utc < k.until_utc;
}

}  // namespace power
