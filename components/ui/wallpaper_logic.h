#pragma once

// The pure half of the power-off wallpaper (host-tested): which files count,
// grey from RGBA, and the 1-bit dither. Wallpapers are PNGs in
// /sdcard/wallpapers (the stock firmware's folder), drawn portrait, centred on
// white (480x800 fills the screen; larger ones are cropped). Atkinson
// dithering: it spreads 6/8 of the error, so light areas stay clean white and
// engravings keep their lines (better than Floyd-Steinberg on the panel).

#include <cstddef>
#include <cstdint>

#include "ui_logic.h"

namespace ui::wallpaper {

inline constexpr const char *kDir = "/sdcard/wallpapers";
inline constexpr size_t kMaxFileBytes = 4u << 20;  // read whole into PSRAM

constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// "*.png", any case; a plain name (also what console `sd put` may write):
// no hidden files (macOS "._name" copies, ".Trash"), no path, no control
// characters.
constexpr bool IsImageName(const char *name) {
    if (name == nullptr || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    size_t n = 0;
    for (; name[n] != '\0'; ++n) {
        if (name[n] == '/' || name[n] == '\\' || static_cast<unsigned char>(name[n]) < 0x20) {
            return false;
        }
    }
    return n > 4 && name[n - 4] == '.' && Lower(name[n - 3]) == 'p' && Lower(name[n - 2]) == 'n' &&
           Lower(name[n - 1]) == 'g';
}

// BT.601 luma, transparent parts on white.
constexpr uint8_t Luma(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint32_t l = (77u * r + 150u * g + 29u * b) >> 8;
    return static_cast<uint8_t>((l * a + 255u * (255u - a)) / 255u);
}

class Dither {
   public:
    // `grey` (w x h, a byte a pixel) centred onto a portrait screen
    // (kScreenBytes, I1: bit 1 white).
    void Run(const uint8_t *grey, uint32_t w, uint32_t h, uint8_t *screen) {
        const int32_t ox = (static_cast<int32_t>(kWidth) - static_cast<int32_t>(w)) / 2;
        const int32_t oy = (static_cast<int32_t>(kHeight) - static_cast<int32_t>(h)) / 2;
        for (auto &row : err_) {
            for (auto &e : row) {
                e = 0;
            }
        }
        for (int32_t y = 0; y < kHeight; ++y) {
            int16_t *cur = err_[y % 3];
            int16_t *next = err_[(y + 1) % 3];
            int16_t *next2 = err_[(y + 2) % 3];
            uint8_t *out = screen + y * kRowBytes;
            const int32_t sy = y - oy;
            for (int32_t x = 0; x < kWidth; ++x) {
                const int32_t sx = x - ox;
                const int32_t src = sy >= 0 && sy < static_cast<int32_t>(h) && sx >= 0 &&
                                            sx < static_cast<int32_t>(w)
                                        ? grey[static_cast<size_t>(sy) * w + sx]
                                        : 255;
                const int32_t v = src + cur[x + 1];
                const bool white = v >= 128;
                const auto bit = static_cast<uint8_t>(0x80 >> (x % 8));
                out[x / 8] = static_cast<uint8_t>(white ? out[x / 8] | bit : out[x / 8] & ~bit);
                const auto e = static_cast<int16_t>((v - (white ? 255 : 0)) / 8);
                cur[x + 2] = static_cast<int16_t>(cur[x + 2] + e);
                cur[x + 3] = static_cast<int16_t>(cur[x + 3] + e);
                next[x] = static_cast<int16_t>(next[x] + e);
                next[x + 1] = static_cast<int16_t>(next[x + 1] + e);
                next[x + 2] = static_cast<int16_t>(next[x + 2] + e);
                next2[x + 1] = static_cast<int16_t>(next2[x + 1] + e);
            }
            for (auto &e : err_[y % 3]) {
                e = 0;  // becomes row y + 3
            }
        }
    }

   private:
    // Three rows of carried error; index x + 1 (one pad left, two right).
    int16_t err_[3][kWidth + 3] = {};
};

}  // namespace ui::wallpaper
