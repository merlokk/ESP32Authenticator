#pragma once

// The pure half of the buttons (host-tested): debounce, short/long press and
// single/double/... press.
// A level and a millisecond clock in, an edge out. Times are uint32_t and only
// ever subtracted, so the ~49-day wrap is not a special case. Debounce and
// PressLength are ported from approver-esp32 (components/buttons).

#include <cstdint>

namespace buttons {

// How long a new level has to hold before it is believed.
inline constexpr uint32_t kDebounceMs = 25;

// Poll period of the blocking helpers; an edge is seen at worst one period
// plus one debounce window late.
inline constexpr uint32_t kPollIntervalMs = 10;

enum class Event : uint8_t {
    kNone = 0,  // nothing changed since the last update
    kPressed,   // released -> pressed, debounced
    kReleased,  // pressed -> released, debounced
};

class Debounce {
public:
    // Adopts a level without an edge: a finger already down at boot is not a
    // press that happened.
    constexpr void Reset(bool pressed, uint32_t now_ms) {
        pressed_ = pressed;
        candidate_ = pressed;
        candidate_since_ms_ = now_ms;
        changed_at_ms_ = now_ms;
    }

    constexpr Event Update(bool raw_pressed, uint32_t now_ms) {
        // Any change of the raw level restarts the window: a bouncing contact
        // never holds one level long enough.
        if (raw_pressed != candidate_) {
            candidate_ = raw_pressed;
            candidate_since_ms_ = now_ms;
        }
        if (raw_pressed == pressed_ || now_ms - candidate_since_ms_ < kDebounceMs) {
            return Event::kNone;
        }
        pressed_ = raw_pressed;
        changed_at_ms_ = now_ms;
        return pressed_ ? Event::kPressed : Event::kReleased;
    }

    constexpr bool Pressed() const { return pressed_; }

    // How long the current state (pressed or released) has held.
    constexpr uint32_t StableMs(uint32_t now_ms) const { return now_ms - changed_at_ms_; }

private:
    bool pressed_ = false;
    bool candidate_ = false;
    uint32_t candidate_since_ms_ = 0;
    uint32_t changed_at_ms_ = 0;
};

enum class Press : uint8_t {
    kNone = 0,
    kShort,  // released before the threshold
    kLong,   // held to the threshold, reported while still held
};

// Short or long press from the debounced level. A long press fires at the
// threshold (feedback while the finger is down); its release reports nothing:
// one press, one action.
class PressLength {
public:
    explicit constexpr PressLength(uint32_t long_ms) : long_ms_(long_ms) {}

    constexpr Press Update(bool pressed, uint32_t now_ms) {
        if (pressed) {
            if (!down_) {
                down_ = true;
                fired_ = false;
                down_at_ms_ = now_ms;
            }
            if (!fired_ && now_ms - down_at_ms_ >= long_ms_) {
                fired_ = true;
                return Press::kLong;
            }
            return Press::kNone;
        }
        const bool was_short = down_ && !fired_;
        down_ = false;
        fired_ = false;
        return was_short ? Press::kShort : Press::kNone;
    }

    // How long the current press has lasted; 0 when released.
    constexpr uint32_t HeldMs(uint32_t now_ms) const { return down_ ? now_ms - down_at_ms_ : 0; }

private:
    uint32_t long_ms_;
    bool down_ = false;
    bool fired_ = false;
    uint32_t down_at_ms_ = 0;
};

// Gap after the last release that ends a series; a press held past the
// longest tap ends it with nothing.
inline constexpr uint32_t kMultiGapMs = 400;
inline constexpr uint32_t kMultiTapMaxMs = 500;

// Single, double, triple... press from the debounced level: the count is
// reported once, `gap_ms` after the last release with no new press. A press
// held longer than `tap_max_ms` cancels the series (a long press is not a tap;
// PressLength reports it). The price: a single press is known only after the
// gap.
class MultiPress {
public:
    explicit constexpr MultiPress(uint32_t gap_ms = kMultiGapMs,
                                  uint32_t tap_max_ms = kMultiTapMaxMs)
        : gap_ms_(gap_ms), tap_max_ms_(tap_max_ms) {}

    // The number of taps when a series ends, else 0.
    constexpr uint8_t Update(bool pressed, uint32_t now_ms) {
        if (pressed) {
            if (!down_) {
                down_ = true;
                down_at_ms_ = now_ms;
            } else if (!cancelled_ && now_ms - down_at_ms_ > tap_max_ms_) {
                cancelled_ = true;
                count_ = 0;
            }
            return 0;
        }
        if (down_) {
            down_ = false;
            if (cancelled_) {
                cancelled_ = false;  // the long press is over; nothing to report
                return 0;
            }
            if (now_ms - down_at_ms_ > tap_max_ms_) {
                count_ = 0;
                return 0;
            }
            if (count_ < 255) {
                ++count_;
            }
            up_at_ms_ = now_ms;
            return 0;
        }
        if (count_ > 0 && now_ms - up_at_ms_ >= gap_ms_) {
            const uint8_t n = count_;
            count_ = 0;
            return n;
        }
        return 0;
    }

    // Taps so far in the running series.
    constexpr uint8_t Pending() const { return count_; }

private:
    uint32_t gap_ms_;
    uint32_t tap_max_ms_;
    bool down_ = false;
    bool cancelled_ = false;
    uint8_t count_ = 0;
    uint32_t down_at_ms_ = 0;
    uint32_t up_at_ms_ = 0;
};

}  // namespace buttons
