#pragma once

// The pure half of the X4 Pro e-ink panel (host-tested): which controller,
// how a frame maps to its RAM, and the command bytes. 800x480, 1 bit per
// pixel, MSB the leftmost pixel, bit 1 white, rows top to bottom: 100 bytes a
// row, 48000 a frame. The controller varies by batch (SSD1677, UC8179,
// UC8279); everything here is from FreeInk (MIT): libs/display/FreeInkDisplay
// (Ssd1677Driver, Uc8179Driver, Uc8279X4Driver, EpdBus) and
// libs/hardware/XteinkDetect.

#include <cstddef>
#include <cstdint>

namespace epd {

inline constexpr uint16_t kWidth = 800;
inline constexpr uint16_t kHeight = 480;
inline constexpr size_t kRowBytes = kWidth / 8;               // 100
inline constexpr size_t kFrameBytes = kRowBytes * kHeight;    // 48000

enum class Controller : uint8_t { kUnknown, kSsd1677, kUc8179, kUc8279 };

constexpr const char *Name(Controller c) {
    switch (c) {
        case Controller::kSsd1677: return "SSD1677";
        case Controller::kUc8179: return "UC8179";
        case Controller::kUc8279: return "UC8279";
        case Controller::kUnknown: break;
    }
    return "unknown";
}

// --- Detection ------------------------------------------------------------------
// The SPI data line is bidirectional: a UC81xx answers FLG (0x71, 1 byte) and
// VER (0x70, 5 bytes) on it; an SSD1677 does not, the line floats to all 0xFF
// (pull-up) or all 0x00. Two passes must agree.

inline constexpr uint8_t kCmdUcVersion = 0x70;
inline constexpr uint8_t kCmdUcFlags = 0x71;
inline constexpr size_t kVersionSize = 5;

constexpr bool Floating(const uint8_t (&ver)[kVersionSize]) {
    for (size_t i = 1; i < kVersionSize; ++i) {
        if (ver[i] != ver[0]) {
            return false;
        }
    }
    return true;
}

// FLG a real status with BUSY_N (bit 0) idle, VER not a floating line.
constexpr bool LooksUc81xx(uint8_t flg, const uint8_t (&ver)[kVersionSize]) {
    return flg != 0x00 && flg != 0xFF && (flg & 0x01) != 0 && !Floating(ver);
}

// LUT_VER (VER byte 2) picks the part; unknown values are a UC8179 (FreeInk).
constexpr Controller UcFromLutVersion(uint8_t lut) {
    switch (lut) {
        case 0x02: case 0x03: case 0x41: case 0x42: case 0x67: case 0x68: case 0x69:
            return Controller::kUc8279;
        default:
            return Controller::kUc8179;
    }
}

// Two passes: both must look like a UC81xx with the same VER, else SSD1677
// (it never answers; an inconclusive probe keeps the default too).
constexpr Controller Detect(uint8_t flg1, const uint8_t (&ver1)[kVersionSize], uint8_t flg2,
                            const uint8_t (&ver2)[kVersionSize]) {
    if (!LooksUc81xx(flg1, ver1) || !LooksUc81xx(flg2, ver2)) {
        return Controller::kSsd1677;
    }
    for (size_t i = 0; i < kVersionSize; ++i) {
        if (ver1[i] != ver2[i]) {
            return Controller::kSsd1677;
        }
    }
    return UcFromLutVersion(ver1[2]);
}

// The stock firmware's NVS hw_calib/screenType: 1 / 0x0B UC8179, 2 / 0x0C
// UC8279, else SSD1677. Shown only: a flash copied from another unit carries
// its value; the probe decides.
constexpr Controller FromScreenType(uint8_t t) {
    return t == 1 || t == 0x0B ? Controller::kUc8179
           : t == 2 || t == 0x0C ? Controller::kUc8279
                                 : Controller::kSsd1677;
}

// --- Frame to RAM -----------------------------------------------------------------
// A UC81xx is addressed as 600 gates with 480 visible; the SSD1677 as 480
// through a RAM window. RamRows rows go out; each is a frame row or padding.

constexpr uint16_t RamRows(Controller c) { return c == Controller::kSsd1677 ? kHeight : 600; }

inline constexpr int kPadRow = -1;  // a row of 0xFF (white)

// Which frame row goes out as RAM row `i`:
// - SSD1677: in order (its window counts Y down).
// - UC8179: bottom row first (PSR SHL set), padding after.
// - UC8279: 120 padding rows first (the gate offset), then in order.
constexpr int FrameRowFor(Controller c, uint16_t i) {
    switch (c) {
        case Controller::kUc8179:
            return i < kHeight ? static_cast<int>(kHeight - 1 - i) : kPadRow;
        case Controller::kUc8279:
            return i < 600 - kHeight ? kPadRow : static_cast<int>(i - (600 - kHeight));
        default:
            return i < kHeight ? static_cast<int>(i) : kPadRow;
    }
}

// --- Busy -------------------------------------------------------------------------
// SSD1677: BUSY high while busy. UC81xx: BUSY_N low while busy.
constexpr bool Busy(Controller c, int level) {
    return c == Controller::kSsd1677 ? level != 0 : level == 0;
}

// --- Commands ---------------------------------------------------------------------
// A command and its data bytes, as FreeInk sends them.
struct Cmd {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[10];
};

// UC8179 (Uc8179Driver.cpp). Planes: 0x10 old, 0x13 new.
inline constexpr Cmd kUc8179Init[] = {
    {0x00, 2, {0x3F, 0x0A}},                    // PSR (OEM 0x3B + SHL)
    {0x61, 4, {0x03, 0x20, 0x02, 0x58}},        // TRES 800 x 600
    {0x65, 4, {0x00, 0x00, 0x00, 0x00}},        // GSST
    {0x03, 1, {0x20}},                          // PFS
    {0x06, 4, {0x25, 0x25, 0x3C, 0x25}},        // booster soft start
    {0xE1, 1, {0x02}},
    {0xE3, 1, {0x22}},                          // power saving
};
inline constexpr Cmd kUc8179Full[] = {
    {0x50, 2, {0x29, 0x07}}, {0xE0, 1, {0x02}}, {0xE5, 1, {0x1E}}, {0x00, 2, {0x1F, 0x0A}},
};
inline constexpr Cmd kUc8179Fast[] = {
    {0x50, 2, {0x29, 0x07}}, {0xE0, 1, {0x02}}, {0xE5, 1, {0x5A}}, {0x00, 2, {0x1F, 0x0A}},
    {0x03, 1, {0x20}},       {0xE1, 1, {0x02}},
};
inline constexpr Cmd kUc8179Idle = {0x50, 2, {0xA9, 0x07}};  // CDI after a refresh

// UC8279, the X4 Pro variant (Uc8279X4Driver.cpp). The PSR with REG cleared
// is written after power on, or it does not latch.
inline constexpr Cmd kUc8279Init[] = {
    {0x00, 2, {0x37, 0x4D}},                    // PSR
    {0x61, 4, {0x03, 0x20, 0x02, 0x58}},        // TRES 800 x 600
    {0x65, 4, {0x00, 0x00, 0x00, 0x00}},        // GSST
    {0x03, 1, {0x20}},                          // PFS
    {0x30, 1, {0x0E}},                          // PLL
    {0xE1, 1, {0x02}},
};
inline constexpr Cmd kUc8279Full[] = {{0x50, 1, {0x97}}, {0xE0, 1, {0x02}}, {0xE5, 1, {0x1E}}};
inline constexpr Cmd kUc8279Fast[] = {
    {0x50, 1, {0xD7}}, {0xE0, 1, {0x02}}, {0xE5, 1, {0x5A}}, {0x03, 1, {0x20}}, {0xE1, 1, {0x02}},
};
inline constexpr Cmd kUc8279Psr = {0x00, 2, {0x17, 0x4D}};
// The fast refresh's window, x 0..799, gates 120..599: without it a fast
// refresh scans but develops nothing.
inline constexpr Cmd kUc8279Window = {
    0x90, 9, {0x00, 0x00, 0x03, 0x1F, 0x00, 0x78, 0x02, 0x57, 0x01}};

inline constexpr uint8_t kUcPowerOn = 0x04;
inline constexpr uint8_t kUcPowerOff = 0x02;
inline constexpr uint8_t kUcRefresh = 0x12;
inline constexpr uint8_t kUcPartialIn = 0x91;
inline constexpr uint8_t kUcPartialOut = 0x92;
inline constexpr uint8_t kUcOld = 0x10;
inline constexpr uint8_t kUcNew = 0x13;
inline constexpr Cmd kUcDeepSleep = {0x07, 1, {0xA5}};

// SSD1677 (Ssd1677Driver.cpp, ssd1677DefaultConfig). After SW reset: 10 ms,
// then BUSY. RAM: 0x24 new (BW), 0x26 old (RED).
inline constexpr Cmd kSsdInit[] = {
    {0x18, 1, {0x80}},                          // internal temperature sensor
    {0x0C, 5, {0xAE, 0xC7, 0xC3, 0xC0, 0x80}},  // booster soft start
    {0x01, 3, {0xDF, 0x01, 0x02}},              // 480 gates, SM
    {0x3C, 1, {0x80}},                          // border
    {0x11, 1, {0x01}},                          // X up, Y down
    {0x44, 4, {0x00, 0x00, 0x1F, 0x03}},        // X 0..799
    {0x45, 4, {0xDF, 0x01, 0x00, 0x00}},        // Y 479..0
};
inline constexpr Cmd kSsdCursor[] = {{0x4E, 2, {0x00, 0x00}}, {0x4F, 2, {0xDF, 0x01}}};
inline constexpr Cmd kSsdFull[] = {{0x21, 1, {0x40}}, {0x3C, 1, {0xC0}}, {0x22, 1, {0xF7}}};
inline constexpr Cmd kSsdFast[] = {{0x21, 1, {0x00}}, {0x3C, 1, {0xC0}}, {0x22, 1, {0xFC}}};
inline constexpr Cmd kSsdPowerOff[] = {{0x3C, 1, {0x80}}, {0x22, 1, {0x03}}};
inline constexpr uint8_t kSsdSwReset = 0x12;
inline constexpr uint8_t kSsdActivate = 0x20;
inline constexpr uint8_t kSsdNew = 0x24;
inline constexpr uint8_t kSsdOld = 0x26;
inline constexpr Cmd kSsdDeepSleep = {0x10, 1, {0x03}};

// Reset: RST high, low, high, 10 ms each; a UC81xx then settles 50 ms more.
inline constexpr uint32_t kResetStepMs = 10;
constexpr uint32_t ResetSettleMs(Controller c) { return c == Controller::kSsd1677 ? 0 : 50; }

// What the bezel hides of the frame, in frame coordinates (landscape, row 0
// on top), measured on our unit with `epd test edges` (4 px steps) held as a
// book: its top is the frame's left (x 0), its sides the frame's top and
// bottom, its bottom the frame's right. FreeInk (T/R/B/L): 9 / 7 / 3 / 7.
inline constexpr uint8_t kInsetTop = 4;
inline constexpr uint8_t kInsetRight = 2;
inline constexpr uint8_t kInsetBottom = 4;
inline constexpr uint8_t kInsetLeft = 8;

inline constexpr uint32_t kBusyTimeoutMs = 30000;
inline constexpr uint32_t kSpiHz = 10000000;  // FreeInk on every X4 Pro batch

}  // namespace epd
