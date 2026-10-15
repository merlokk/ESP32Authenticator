#pragma once

// The pure half of the buttons (host-tested): debounce and short/long press.
// A level and a millisecond clock in, an edge out. Times are uint32_t and only
// ever subtracted, so the ~49-day wrap is not a special case. Ported from
// approver-esp32 (components/buttons).

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

}  // namespace buttons
