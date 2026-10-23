#pragma once

// The pure half of the GT911 touch controller (host-tested): registers, the
// power-up and reset timing, and the Home key state. X4 Pro wiring and
// sequence from FreeInk (docs/xteink-x4pro-support.md, InputManager
// beginGt911/pollGt911): power GPIO2 active low, RST GPIO4, INT GPIO10; the
// controller loads its own config after a long enough reset.

#include <cstdint>

namespace touch::gt911 {

// INT low while RST rises selects 0x5D, high selects 0x14.
inline constexpr uint8_t kAddress = 0x5D;
inline constexpr uint8_t kAddressAlt = 0x14;

// 16-bit register addresses, sent high byte first. Read-only here except the
// status clear: the config (0x8047..0x8100) is the module's, never written.
inline constexpr uint16_t kRegConfig = 0x8047;     // config version, then the config
inline constexpr uint16_t kRegProductId = 0x8140;  // "911\0", then the info block
inline constexpr uint16_t kRegStatus = 0x814E;     // written 0 after every frame
inline constexpr uint16_t kRegPoints = 0x8150;     // 8 bytes per contact
inline constexpr uint8_t kMaxPoints = 5;
inline constexpr uint8_t kPointSize = 8;

// The info block from 0x8140: product id, firmware, resolution, vendor.
inline constexpr uint8_t kInfoSize = 11;
// The config head from 0x8047: version, X/Y max, touch number, switches.
inline constexpr uint8_t kConfigHeadSize = 7;

struct Info {
    char product[5];     // "911"
    uint16_t firmware;   // 0x8144, little endian
    uint16_t x_max;      // 0x8146
    uint16_t y_max;      // 0x8148
    uint8_t vendor;      // 0x814A: the sensor/module id
};

constexpr Info DecodeInfo(const uint8_t (&raw)[kInfoSize]) {
    Info out = {};
    for (int i = 0; i < 4; ++i) {
        const auto c = static_cast<char>(raw[i]);
        out.product[i] = c >= 0x20 && c < 0x7F ? c : '\0';
    }
    out.product[4] = '\0';
    out.firmware = static_cast<uint16_t>(raw[4] | (raw[5] << 8));
    out.x_max = static_cast<uint16_t>(raw[6] | (raw[7] << 8));
    out.y_max = static_cast<uint16_t>(raw[8] | (raw[9] << 8));
    out.vendor = raw[10];
    return out;
}

struct ConfigHead {
    uint8_t version;     // 0x8047: 'A'.. ; 0 = the module's own default
    uint16_t x_max;      // 0x8048, the output range
    uint16_t y_max;      // 0x804A
    uint8_t touches;     // 0x804C: max contacts, 1..5 (low nibble)
    uint8_t switch1;     // 0x804D: INT trigger (bits 0-1), X2Y (bit 3), ...
};

constexpr ConfigHead DecodeConfigHead(const uint8_t (&raw)[kConfigHeadSize]) {
    return {raw[0], static_cast<uint16_t>(raw[1] | (raw[2] << 8)),
            static_cast<uint16_t>(raw[3] | (raw[4] << 8)), static_cast<uint8_t>(raw[5] & 0x0F),
            raw[6]};
}

// One contact record. X4 Pro: the coordinates start at byte 0 (no track id;
// FreeInk gt911CoordsAtByte0), the panel is mounted portrait (X 0..480, Y
// 0..800 on the 800x480 landscape panel).
struct Point {
    uint16_t x;
    uint16_t y;
    uint16_t size;
};

constexpr Point DecodePoint(const uint8_t *record) {
    return {static_cast<uint16_t>(record[0] | (record[1] << 8)),
            static_cast<uint16_t>(record[2] | (record[3] << 8)),
            static_cast<uint16_t>(record[4] | (record[5] << 8))};
}

inline constexpr uint8_t kStatusReady = 0x80;  // a new frame
inline constexpr uint8_t kStatusKey = 0x10;    // the Home key is touched
inline constexpr uint8_t kStatusCount = 0x0F;  // contacts on the panel

// Power on, then the reset with INT at the address level.
inline constexpr uint32_t kPowerSettleMs = 50;
inline constexpr uint32_t kResetLowMs = 10;
inline constexpr uint32_t kResetHighMs = 10;
inline constexpr uint32_t kIntHoldMs = 50;
inline constexpr uint32_t kConfigLoadMs = 50;

struct Frame {
    bool fresh;       // a new frame: clear the status after it
    bool home;        // the Home key, latched across frames
    uint8_t touches;  // contacts in this frame (0 when not fresh)
};

// From the status byte and the Home state so far. A motionless key makes no
// new frames, so the state only changes on a fresh one; between frames it
// stays as it was.
constexpr Frame Decode(uint8_t status, bool home_before) {
    if ((status & kStatusReady) == 0) {
        return {false, home_before, 0};
    }
    return {true, (status & kStatusKey) != 0, static_cast<uint8_t>(status & kStatusCount)};
}

}  // namespace touch::gt911
