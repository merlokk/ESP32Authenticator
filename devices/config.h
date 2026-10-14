#pragma once

// Build-time hardware choice: the board this firmware is built for. Everything
// else (clock, display, touch, LED, SD, pins) follows from the board's class in
// boards/. Change kBoardId and rebuild.
//
// Included by path ("../config.h" from boards/), not on any include path:
// components/config/config.h is the config.json layer.

#include <cstdint>

namespace boards {

enum class BoardId : uint8_t {
    kDev,    // ESP32-S3 dev board: BOOT button, WS2812 LED (boards/dev_board.h)
    kX4Pro,  // XTEINK X4 Pro (boards/x4_pro.h)
};

inline constexpr BoardId kBoardId = BoardId::kDev;

}  // namespace boards
