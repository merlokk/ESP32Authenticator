#pragma once

// The pure half of the WS2812 LED (host-tested): colours, rhythms, the breath
// ramp, and the UART encoding of one pixel. The peripheral and the task are
// led.h. Ported from approver-esp32-yubikey.

#include <cstddef>
#include <cstdint>

namespace led {

// Written RGB; the wire wants GRB (EncodePixel swaps).
struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    constexpr bool operator==(const Rgb &other) const {
        return r == other.r && g == other.g && b == other.b;
    }
    constexpr bool operator!=(const Rgb &other) const { return !(*this == other); }
    constexpr bool Dark() const { return r == 0 && g == 0 && b == 0; }
};

// Compiled in: which colour means what is the interface, not a setting.
namespace colour {
inline constexpr Rgb kOff{0, 0, 0};
inline constexpr Rgb kWhite{255, 255, 255};
inline constexpr Rgb kRed{255, 0, 0};
inline constexpr Rgb kGreen{0, 255, 0};
inline constexpr Rgb kBlue{0, 0, 255};
inline constexpr Rgb kAmber{255, 140, 0};
inline constexpr Rgb kYellow{255, 210, 0};
inline constexpr Rgb kCyan{0, 255, 255};
inline constexpr Rgb kMagenta{255, 0, 255};
}  // namespace colour

enum class Effect : uint8_t {
    kSolid = 0,  // on
    kBeacon,     // 100 ms in every 2 s
    kSlowBlink,  // 1 s / 1 s
    kNormBlink,  // 500 / 500 ms
    kFastBlink,  // 200 / 200 ms
    kBreathe,    // the ramp below, 9 s a cycle
};

const char *EffectName(Effect effect);

// Duty cycle for a breath that is linear in *perceived* lightness (CIE L*
// walked 0..100..0, solved for duty): crowded at the bottom on purpose.
// 180 steps x 50 ms = 9 s.
inline constexpr size_t kBreathSteps = 180;
extern const uint8_t kBreathRamp[kBreathSteps];
inline constexpr uint32_t kBreathStepMs = 50;

// Brightness ceiling in percent, proportional (not gamma): 1 % of 255 is 2.
Rgb Scale(Rgb colour, uint8_t percent);

// What to show at a given millisecond, and how long that frame is good for
// (the task sleeps that long). Times are wrap-safe uint32_t milliseconds.
class Animator {
public:
    // Restarts the phase only when something changed: re-asserting the same
    // state must not freeze a breath at its first step.
    void Set(Rgb colour, Effect effect, uint8_t percent, uint32_t now_ms);

    // Shown for `duration_ms`, then back to what Set() last asked for.
    void SetFor(Rgb colour, Effect effect, uint8_t percent, uint32_t duration_ms,
                uint32_t now_ms);

    // Ends a running SetFor now (a prompt that got its answer). No-op otherwise.
    void EndFor(uint32_t now_ms);

    Rgb FrameAt(uint32_t now_ms, uint32_t *next_ms);

    Rgb Colour() const { return overriding_ ? over_colour_ : colour_; }
    Effect CurrentEffect() const { return overriding_ ? over_effect_ : effect_; }
    uint8_t Percent() const { return overriding_ ? over_percent_ : percent_; }
    bool Overriding() const { return overriding_; }

private:
    Rgb colour_{};
    Effect effect_ = Effect::kSolid;
    uint8_t percent_ = 0;

    bool overriding_ = false;
    Rgb over_colour_{};
    Effect over_effect_ = Effect::kSolid;
    uint8_t over_percent_ = 0;
    uint32_t over_until_ms_ = 0;

    uint32_t phase_at_ms_ = 0;
};

// --- The wire -------------------------------------------------------------------
// WS2812 from a UART: 3,333,333 baud, 6 data bits, TX inverted (idles low) gives
// 8 bit-times of 300 ns per character = two WS2812 bit cells. Each pair of
// colour bits is one character: 00 -> 0x37, 01 -> 0x07, 10 -> 0x34, 11 -> 0x04.
inline constexpr size_t kBytesPerPixel = 12;

// GRB on the wire. Returns bytes written, 0 when `capacity` is short.
size_t EncodePixel(Rgb colour, uint8_t *out, size_t capacity);

}  // namespace led
