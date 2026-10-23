#pragma once

// The X4 Pro e-ink panel, 800x480 B/W (epd_logic.h: the controller, the frame
// format, the command bytes). The controller is found at Init (SSD1677,
// UC8179 or UC8279); nothing is drawn until Refresh, so the picture already
// on the glass stays. One caller at a time (a mutex).

#include <cstdint>

#include "epd_logic.h"
#include "esp_err.h"

namespace epd {

enum class Mode : uint8_t {
    kFull,  // clears ghosting, ~1-2 s with flashing
    kFast,  // the changed pixels only; the first refresh after Init is full anyway
};

// After boards::Init. Detects the controller, starts SPI, initialises it.
esp_err_t Init();
bool Ready();
Controller Detected();

// Draws a frame (kFrameBytes: 100 bytes a row, MSB left, bit 1 white) and
// waits for the panel; it is powered off after. ESP_ERR_TIMEOUT: BUSY never
// settled.
esp_err_t Refresh(const uint8_t *frame, Mode mode);

// 4 grey levels (HasGrey: UC8179 only, else ESP_ERR_NOT_SUPPORTED), one full
// refresh; for still pictures. Two frames in the Refresh format: `low` the
// low bit of each pixel's level, `high` the high bit (0 black .. 3 white).
// The next Refresh is a full one, after a pass that clears the grey.
esp_err_t RefreshGrey(const uint8_t *low, const uint8_t *high);

// Deep sleep: the panel keeps its picture without power; Refresh wakes it
// (reset and init again).
esp_err_t Sleep();

// Console `epd`: controller, how it was found, the stock firmware's
// screenType, refresh count and the last duration.
void PrintInfo();

}  // namespace epd
