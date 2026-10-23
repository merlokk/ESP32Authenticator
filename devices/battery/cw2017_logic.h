#pragma once

// The pure half of the CW2017 fuel gauge (host-tested): registers, the X4 Pro
// battery profile, the VCELL formula and the init decisions. Register map,
// profile and sequence are from the stock firmware (XTEink::Cw2017PowerHal),
// as recovered by FreeInk (libs/hardware/BatteryMonitor, MIT); the same 80
// bytes are in stock 7.5.11 (originalfw/).

#include <cstddef>
#include <cstdint>

namespace battery::cw2017 {

inline constexpr uint8_t kAddress = 0x63;

inline constexpr uint8_t kRegVersion = 0x00;   // 0xA0 while starting, 0x0D/0x0F running
inline constexpr uint8_t kRegVcell = 0x02;     // 14 bits, big-endian over 0x02/0x03
inline constexpr uint8_t kRegSoc = 0x04;       // integer percent
inline constexpr uint8_t kRegSocFrac = 0x05;   // 1/256 percent
inline constexpr uint8_t kRegMode = 0x08;      // soft reset / sleep
inline constexpr uint8_t kRegSocAlert = 0x0B;  // bit 7: the profile is loaded
inline constexpr uint8_t kRegBatinfo = 0x10;   // the profile, 0x10..0x5F

inline constexpr uint8_t kModeNormal = 0x00;
inline constexpr uint8_t kModeRestart = 0x30;
inline constexpr uint8_t kModeDefault = 0xF0;
inline constexpr uint8_t kUpdateFlag = 0x80;

// Soft reset: MODE kModeDefault -> kModeRestart -> kModeNormal, this far apart.
inline constexpr uint32_t kResetStepMs = 20;
// After a reset: VERSION running within 50 x 20 ms, then a valid SoC within
// 30 x 100 ms (the vendor drivers' bounds).
inline constexpr int kVersionPolls = 50;
inline constexpr uint32_t kVersionPollMs = 20;
inline constexpr int kSocPolls = 30;
inline constexpr uint32_t kSocPollMs = 100;

inline constexpr size_t kBatinfoSize = 80;

// The profile of the X4 Pro's cell (stock firmware table). The gauge reports
// 0 % until it is loaded; it keeps it while the cell is connected.
inline constexpr uint8_t kBatinfo[kBatinfoSize] = {
    0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0xbf, 0xb5, 0xb4, 0xa4, 0x9c, 0xeb, 0xe2,
    0xdf, 0xe5, 0xca, 0xa0, 0x8a, 0x62, 0x53, 0x48, 0x40, 0x3a, 0x32, 0xb1, 0xae, 0xda, 0xb5, 0xff,
    0xff, 0xff, 0xe8, 0xdb, 0xd9, 0xd6, 0xd4, 0xd2, 0xd0, 0xcb, 0xc3, 0xbc, 0x9e, 0x87, 0x7b, 0x71,
    0x72, 0x7c, 0x8c, 0xa3, 0xb7, 0xc8, 0xa5, 0x4f, 0x00, 0x00, 0xab, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23};

// The calculation engine runs (not the 0xA0 startup state).
constexpr bool VersionRunning(uint8_t version) { return (version & 0xFD) == 0x0D; }

constexpr bool SocValid(uint8_t soc) { return soc <= 100; }

// VCELL registers 0x02 (high) and 0x03 (low) to millivolts: 14 bits of
// 312.5 uV, rounded (the stock formula).
constexpr uint16_t VcellMillivolts(uint8_t high, uint8_t low) {
    const uint32_t raw = (static_cast<uint32_t>(high & 0x3F) << 8) | low;
    return static_cast<uint16_t>((raw * 5 + 8) >> 4);
}

// The resident profile is ours: the update flag is set and every byte matches.
constexpr bool ProfileMatches(uint8_t soc_alert, const uint8_t (&resident)[kBatinfoSize]) {
    if ((soc_alert & kUpdateFlag) == 0) {
        return false;
    }
    for (size_t i = 0; i < kBatinfoSize; ++i) {
        if (resident[i] != kBatinfo[i]) {
            return false;
        }
    }
    return true;
}

// What init does to the gauge, from what it reads first.
enum class Action : uint8_t {
    kNone,     // running with our profile: only read
    kRestart,  // our profile, but asleep or starting: soft reset
    kUpload,   // other or no profile: write it, set the flag, soft reset
};

constexpr Action Decide(uint8_t mode, uint8_t version, uint8_t soc, bool profile_matches) {
    if (!profile_matches) {
        return Action::kUpload;
    }
    if (mode != kModeNormal || !VersionRunning(version) || !SocValid(soc)) {
        return Action::kRestart;
    }
    return Action::kNone;
}

constexpr const char *Name(Action a) {
    switch (a) {
        case Action::kNone: return "none";
        case Action::kRestart: return "restart";
        case Action::kUpload: return "profile upload";
    }
    return "?";
}

}  // namespace battery::cw2017
