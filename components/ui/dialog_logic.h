#pragma once

// The pure half of the dialogs (dialog.h, host-tested): a held Yes. It counts
// only while held: kHoldMs held is an answer, let go (or slid off) earlier:
// empty again. Its fill grows in kHoldSteps (an e-ink refresh each).

#include <cstdint>

namespace ui::dialog {

inline constexpr uint32_t kHoldMs = 4000;
inline constexpr uint8_t kHoldSteps = 8;

class Hold {
   public:
    constexpr bool done() const { return done_; }

    // Pressed at `now` (ms), let go.
    constexpr void Press(uint32_t now) {
        if (!done_) {
            holding_ = true;
            from_ = now;
        }
    }
    constexpr void Release() { holding_ = false; }
    constexpr void Reset() {
        holding_ = false;
        done_ = false;
    }

    // The fill at `now`, 0..kHoldSteps; kHoldSteps: held long enough (done).
    constexpr uint8_t Fill(uint32_t now) {
        if (done_) {
            return kHoldSteps;
        }
        if (!holding_) {
            return 0;
        }
        const uint32_t held = now - from_;  // the tick may wrap
        if (held >= kHoldMs) {
            done_ = true;
            holding_ = false;
            return kHoldSteps;
        }
        return static_cast<uint8_t>(held * kHoldSteps / kHoldMs);
    }

   private:
    bool holding_ = false;
    bool done_ = false;
    uint32_t from_ = 0;
};

// A firmware update's window (ui::ShowUpdate): the bar moves in kUpdateStep %
// steps, an e-ink refresh each.
inline constexpr uint8_t kUpdateStep = 5;

constexpr uint8_t UpdatePercent(uint32_t written, uint32_t total) {
    if (total == 0) {
        return 0;
    }
    return written >= total ? 100 : static_cast<uint8_t>(uint64_t{written} * 100 / total);
}

// Redraw for `percent` when `shown` (-1: nothing yet) is in another step.
constexpr bool UpdateRedraw(int shown, uint8_t percent) {
    return shown < 0 || percent / kUpdateStep != shown / kUpdateStep;
}

}  // namespace ui::dialog
