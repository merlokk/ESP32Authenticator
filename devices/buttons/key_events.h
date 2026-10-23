#pragma once

// What one button did, from its raw level (host-tested): the debounced edges
// for an instant reaction, then the gesture: a long press (fired while still
// held) or a series of clicks (counted when the series ends). A press is one
// or the other, never both: held past kLongMs it is long, else a click.
// Built from button_logic.h.

#include <cstdint>

#include "button_logic.h"

namespace buttons {

inline constexpr uint32_t kLongMs = 600;
// The longest click: one below kLongMs, so a press at exactly kLongMs is long only.
inline constexpr uint32_t kTapMaxMs = kLongMs - 1;

enum class Kind : uint8_t {
    kPressed = 1,  // debounced down
    kReleased,     // debounced up
    kLong,         // held kLongMs, fired while held; its release is only kReleased
    kClick,        // `count` presses in a row, kMultiGapMs after the last release
};

struct KeyEvent {
    uint8_t key = 0;  // the caller's button number
    Kind kind = Kind::kPressed;
    uint8_t count = 0;  // kClick: 1 single, 2 double, ...
};

// At most two events per update (an edge and a gesture). Times are uint32_t
// milliseconds, only ever subtracted.
struct KeyEvents {
    KeyEvent items[2];
    uint8_t size = 0;

    constexpr void Add(const KeyEvent &e) {
        if (size < 2) {
            items[size++] = e;
        }
    }
};

class KeyTracker {
public:
    constexpr explicit KeyTracker(uint8_t key = 0) : key_(key) {}

    // A level already there (a finger down at boot) is no press.
    constexpr void Reset(bool raw_pressed, uint32_t now_ms) {
        debounce_.Reset(raw_pressed, now_ms);
        length_ = PressLength(kLongMs);
        clicks_ = MultiPress(kMultiGapMs, kTapMaxMs);
        ignore_until_release_ = raw_pressed;
    }

    constexpr KeyEvents Update(bool raw_pressed, uint32_t now_ms) {
        KeyEvents out;
        const Event edge = debounce_.Update(raw_pressed, now_ms);
        if (edge == Event::kPressed) {
            out.Add({key_, Kind::kPressed, 0});
        } else if (edge == Event::kReleased) {
            out.Add({key_, Kind::kReleased, 0});
        }
        const bool down = debounce_.Pressed();
        if (ignore_until_release_) {
            ignore_until_release_ = down;
            return out;
        }
        if (length_.Update(down, now_ms) == Press::kLong) {
            out.Add({key_, Kind::kLong, 0});
        }
        const uint8_t n = clicks_.Update(down, now_ms);
        if (n > 0) {
            out.Add({key_, Kind::kClick, n});
        }
        return out;
    }

    constexpr bool Pressed() const { return debounce_.Pressed(); }

    // How long the current press has lasted (debounced); 0 when released.
    constexpr uint32_t HeldMs(uint32_t now_ms) const {
        return debounce_.Pressed() ? debounce_.StableMs(now_ms) : 0;
    }

private:
    uint8_t key_;
    Debounce debounce_;
    PressLength length_{kLongMs};
    MultiPress clicks_{kMultiGapMs, kTapMaxMs};
    bool ignore_until_release_ = false;
};

constexpr const char *Name(Kind k) {
    switch (k) {
        case Kind::kPressed: return "pressed";
        case Kind::kReleased: return "released";
        case Kind::kLong: return "long";
        case Kind::kClick: return "click";
    }
    return "?";
}

}  // namespace buttons
