#pragma once

// The board's touch controller (X4 Pro: GT911 on the leased I2C bus,
// gt911_logic.h). The keypad task polls it (one owner: a frame is read and
// cleared once): the capacitive Home key becomes the keypad's `home`, the
// panel contacts are kept as the last frame (the UI comes later).

#include <cstdint>

#include "esp_err.h"
#include "gt911_logic.h"

namespace touch {

// After boards::Init (the peripheral rail and the I2C bus). Powers the
// controller, resets it, finds it at 0x5D or 0x14, reads its info.
// ESP_ERR_NOT_FOUND: no answer.
esp_err_t Init();
bool Present();

// One status read; after a new frame its contacts, then the clear. Updates
// the Home state. From the keypad task only.
esp_err_t Poll();

// As of the last Poll().
bool HomePressed();

// The last frame with contacts, and how many frames so far.
struct Snapshot {
    uint32_t frames;  // new frames seen
    uint32_t errors;  // failed reads
    uint8_t status;   // of the last new frame
    uint8_t count;    // contacts in it (up to gt911::kMaxPoints read)
    uint8_t raw[gt911::kMaxPoints * gt911::kPointSize];
};
Snapshot GetSnapshot();

// Console `touch`: address, product, firmware, resolution, vendor, config head.
void PrintInfo();

}  // namespace touch
