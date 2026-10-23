#pragma once

// The pure half of the frontlight (host-tested): two LED strings, cool and
// warm, each on a PWM pin. Brightness is the total light along a perceptual
// curve; warmth splits that total between the strings, so the light keeps its
// level while its tint changes. From FreeInk (FrontlightManager): 25 kHz,
// 10 bits as stock 7.x, gamma 1.6554 so 1 % is the dimmest step (1 LSB) and
// every percent is a distinct duty.

#include <cstdint>

namespace frontlight {

inline constexpr uint32_t kPwmHz = 25000;
inline constexpr uint32_t kResolutionBits = 10;
inline constexpr uint32_t kFullDuty = (1u << kResolutionBits) - 1;  // 1023
// A change fades over this long (0 -> full or back: no flash in the dark).
inline constexpr uint32_t kFadeMs = 250;

// Duty for 0..100 % brightness: max(1, round(1023 * (p/100)^1.6554)), 0 for 0.
inline constexpr uint16_t kCurve[101] = {
    0,    1,    2,    3,    5,    7,    10,   13,   16,   19,   23,   26,   31,   35,
    39,   44,   49,   54,   60,   65,   71,   77,   83,   90,   96,   103,  110,  117,
    124,  132,  139,  147,  155,  163,  172,  180,  189,  197,  206,  215,  224,  234,
    243,  253,  263,  273,  283,  293,  304,  314,  325,  336,  347,  358,  369,  380,
    392,  403,  415,  427,  439,  451,  464,  476,  489,  501,  514,  527,  540,  553,
    567,  580,  594,  608,  621,  635,  649,  664,  678,  692,  707,  722,  737,  751,
    767,  782,  797,  812,  828,  844,  859,  875,  891,  907,  923,  940,  956,  973,
    989,  1006, 1023};

struct Duties {
    uint16_t cool;
    uint16_t warm;
};

// `brightness` and `warmth` in percent (above 100: 100). Warmth 0 is all
// cool, 100 all warm. The total is split, not each percentage: brightness 1 at
// warmth 50 is still 1 LSB of light, not 0 + 0.
constexpr Duties Mix(uint8_t brightness, uint8_t warmth) {
    const uint32_t total = kCurve[brightness > 100 ? 100 : brightness];
    const uint32_t w = warmth > 100 ? 100 : warmth;
    const auto warm = static_cast<uint16_t>((total * w + 50) / 100);
    return {static_cast<uint16_t>(total - warm), warm};
}

}  // namespace frontlight
