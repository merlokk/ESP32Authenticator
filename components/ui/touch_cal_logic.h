#pragma once

// The touch's calibration (Settings > Touchscreen, x4-design.md): five
// crosses tapped in turn; per axis a straight line through the taps (least
// squares), screen = raw * scale / 1000 + offset. A fit far from the glass
// (scale off by over 10 %, offset over 80 px, a tap over 30 px off its line)
// is refused: tapped beside a cross. Pure (host-tested).

#include <cstddef>
#include <cstdint>

#include "ui_logic.h"

namespace ui::touch_cal {

struct Calibration {
    int16_t x_scale = 1000;  // per mille
    int16_t x_offset = 0;    // px
    int16_t y_scale = 1000;
    int16_t y_offset = 0;
};

constexpr bool IsDefault(const Calibration &c) {
    return c.x_scale == 1000 && c.x_offset == 0 && c.y_scale == 1000 && c.y_offset == 0;
}

constexpr uint16_t Clamp(int32_t v, uint16_t size) {
    return static_cast<uint16_t>(v < 0 ? 0 : v >= size ? size - 1 : v);
}

// A raw point (TouchToScreen, uncalibrated) to the screen.
constexpr ScreenPoint Apply(const Calibration &c, ScreenPoint raw) {
    return {Clamp(raw.x * c.x_scale / 1000 + c.x_offset, kWidth),
            Clamp(raw.y * c.y_scale / 1000 + c.y_offset, kHeight)};
}

// The crosses, in order: the corners (clear of the title bar), the middle.
inline constexpr uint8_t kTargets = 5;
inline constexpr ScreenPoint kTarget[kTargets] = {
    {40, 160}, {440, 160}, {440, 760}, {40, 760}, {240, 460}};

inline constexpr int32_t kMaxScaleOff = 100;  // per mille
inline constexpr int32_t kMaxOffset = 80;
inline constexpr int32_t kMaxMiss = 30;

struct Fit {
    bool ok;
    Calibration calibration;
    int32_t worst;  // px, the farthest tap from its line
};

struct Line {
    bool ok;
    int32_t scale;
    int32_t offset;
};

// target = raw * scale / 1000 + offset over n points.
constexpr Line FitAxis(const int32_t *raw, const int32_t *target, size_t n) {
    int64_t sr = 0;
    int64_t st = 0;
    int64_t srr = 0;
    int64_t srt = 0;
    for (size_t i = 0; i < n; ++i) {
        sr += raw[i];
        st += target[i];
        srr += static_cast<int64_t>(raw[i]) * raw[i];
        srt += static_cast<int64_t>(raw[i]) * target[i];
    }
    const int64_t den = static_cast<int64_t>(n) * srr - sr * sr;
    if (den == 0) {
        return {false, 1000, 0};
    }
    const int64_t num = static_cast<int64_t>(n) * srt - sr * st;
    const int64_t scale = (num * 1000 + (num >= 0 ? den / 2 : -den / 2)) / den;
    const int64_t rest = st * 1000 - scale * sr;  // offset * n * 1000
    const int64_t div = static_cast<int64_t>(n) * 1000;
    const int64_t offset = (rest + (rest >= 0 ? div / 2 : -div / 2)) / div;
    return {true, static_cast<int32_t>(scale), static_cast<int32_t>(offset)};
}

constexpr int32_t Abs(int32_t v) { return v < 0 ? -v : v; }

// The raw taps on kTarget, in order.
constexpr Fit Solve(const ScreenPoint *taps) {
    int32_t rx[kTargets] = {};
    int32_t ry[kTargets] = {};
    int32_t tx[kTargets] = {};
    int32_t ty[kTargets] = {};
    for (uint8_t i = 0; i < kTargets; ++i) {
        rx[i] = taps[i].x;
        ry[i] = taps[i].y;
        tx[i] = kTarget[i].x;
        ty[i] = kTarget[i].y;
    }
    const Line x = FitAxis(rx, tx, kTargets);
    const Line y = FitAxis(ry, ty, kTargets);
    Fit f = {false, {}, 0};
    if (!x.ok || !y.ok) {
        return f;
    }
    f.calibration = {static_cast<int16_t>(x.scale), static_cast<int16_t>(x.offset),
                     static_cast<int16_t>(y.scale), static_cast<int16_t>(y.offset)};
    for (uint8_t i = 0; i < kTargets; ++i) {
        const int32_t dx = rx[i] * x.scale / 1000 + x.offset - tx[i];
        const int32_t dy = ry[i] * y.scale / 1000 + y.offset - ty[i];
        f.worst = Abs(dx) > f.worst ? Abs(dx) : f.worst;
        f.worst = Abs(dy) > f.worst ? Abs(dy) : f.worst;
    }
    f.ok = Abs(x.scale - 1000) <= kMaxScaleOff && Abs(y.scale - 1000) <= kMaxScaleOff &&
           Abs(x.offset) <= kMaxOffset && Abs(y.offset) <= kMaxOffset && f.worst <= kMaxMiss;
    return f;
}

}  // namespace ui::touch_cal
