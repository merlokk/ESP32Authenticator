#pragma once

// The pure half of the UI (host-tested): the portrait screen and how it lands
// on the landscape panel. LVGL draws 480x800 portrait, 1 bit per pixel (I1:
// MSB the leftmost pixel, bit 1 white, 60 bytes a row); the panel takes
// 800x480 landscape frames (epd_logic.h). Held as a book, the portrait top is
// the frame's left (epd_logic.h insets): the picture turns 90 degrees
// clockwise, portrait (x, y) is frame (y, 479 - x).

#include <cstddef>
#include <cstdint>

#include "epd_logic.h"

namespace ui {

inline constexpr uint16_t kWidth = epd::kHeight;   // 480
inline constexpr uint16_t kHeight = epd::kWidth;   // 800
inline constexpr size_t kRowBytes = kWidth / 8;    // 60
inline constexpr size_t kScreenBytes = kRowBytes * kHeight;  // 48000

// What the bezel hides, in portrait coordinates.
inline constexpr uint8_t kInsetTop = epd::kInsetLeft;
inline constexpr uint8_t kInsetBottom = epd::kInsetRight;
inline constexpr uint8_t kInsetLeft = epd::kInsetBottom;
inline constexpr uint8_t kInsetRight = epd::kInsetTop;

constexpr uint16_t PanelX(uint16_t x, uint16_t y) {
    (void)x;
    return y;
}
constexpr uint16_t PanelY(uint16_t x, uint16_t y) {
    (void)y;
    return static_cast<uint16_t>(epd::kHeight - 1 - x);
}

// A portrait screen (kScreenBytes) to a panel frame (epd::kFrameBytes). Frame
// row fy is portrait column 479 - fy; its 8-pixel bytes are 8 portrait rows.
inline void ToPanel(const uint8_t *portrait, uint8_t *frame) {
    for (uint16_t fy = 0; fy < epd::kHeight; ++fy) {
        const uint16_t x = static_cast<uint16_t>(epd::kHeight - 1 - fy);
        const uint8_t *col = portrait + x / 8;
        const auto mask = static_cast<uint8_t>(0x80 >> (x % 8));
        uint8_t *out = frame + fy * epd::kRowBytes;
        for (size_t fb = 0; fb < epd::kRowBytes; ++fb) {
            uint8_t b = 0;
            for (uint8_t k = 0; k < 8; ++k) {
                if (col[(fb * 8 + k) * kRowBytes] & mask) {
                    b = static_cast<uint8_t>(b | (0x80 >> k));
                }
            }
            out[fb] = b;
        }
    }
}

// A GT911 contact (gt911_logic.h: X 0..479, Y 0..799, the sensor is mounted
// portrait) to the portrait screen; clamped, so a reading at the edge stays on
// it. The flips are measured on the X4 Pro.
inline constexpr bool kTouchFlipX = false;
inline constexpr bool kTouchFlipY = false;

struct ScreenPoint {
    uint16_t x;
    uint16_t y;
};

constexpr ScreenPoint TouchToScreen(uint16_t tx, uint16_t ty) {
    const uint16_t x = tx < kWidth ? tx : kWidth - 1;
    const uint16_t y = ty < kHeight ? ty : kHeight - 1;
    return {kTouchFlipX ? static_cast<uint16_t>(kWidth - 1 - x) : x,
            kTouchFlipY ? static_cast<uint16_t>(kHeight - 1 - y) : y};
}

// Pixels that differ between two frames of `bytes` bytes.
constexpr uint32_t ChangedPixels(const uint8_t *a, const uint8_t *b, size_t bytes) {
    uint32_t n = 0;
    for (size_t i = 0; i < bytes; ++i) {
        for (uint8_t d = static_cast<uint8_t>(a[i] ^ b[i]); d != 0; d &= static_cast<uint8_t>(d - 1)) {
            ++n;
        }
    }
    return n;
}

// Fast refreshes leave ghosts: every kFullEvery-th refresh is a full one
// (and the first, though the driver forces that itself). A fast refresh only
// nudges the pixels: a new screen in one (the PIN pad over the boot screen)
// came out faint and darkened tap by tap (29524 px changed; a tap changes a
// dot, ~600 px, and a line), so a frame that changes kBigChange
// pixels or more is a full one too, and the count starts over.
class RefreshPlan {
   public:
    static constexpr uint16_t kFullEvery = 20;
    static constexpr uint32_t kBigChange = epd::kWidth * epd::kHeight / 32;  // 12000

    bool NextIsFull(uint32_t changed = 0) {
        const bool full = count_ == 0 || changed >= kBigChange;
        count_ = static_cast<uint16_t>(((full ? 0 : count_) + 1) % kFullEvery);
        return full;
    }

   private:
    uint16_t count_ = 0;
};

}  // namespace ui
