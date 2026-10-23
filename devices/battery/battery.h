#pragma once

// The board's battery: voltage and level from the fuel gauge (CW2017 on the
// X4 Pro, cw2017_logic.h), charging from the charger's STAT line. Without
// either, the readout says so.

#include <cstdint>

#include "esp_err.h"

namespace battery {

enum class Charge : uint8_t {
    kUnknown,      // no STAT line
    kCharging,     // STAT high
    kNotCharging,  // no charger, or the cell is full
};

struct Reading {
    uint16_t millivolts = 0;
    uint8_t percent = 0;   // 0..100
    uint8_t fraction = 0;  // 1/256 percent
};

// After boards::Init (the I2C bus): the STAT pin, then the gauge. Loads the
// profile only when the gauge holds another one, which takes up to ~4 s.
// Not fatal: the readouts report what is missing.
esp_err_t Init();

// The gauge answered at Init and runs with our profile.
bool GaugeReady();

// One transaction. ESP_ERR_INVALID_STATE: the gauge stopped (`battery init`).
esp_err_t Read(Reading *out);

Charge ChargeState();

// 0..100 for the BLE Battery Level, -1 without a reading.
int Level();

// Init again (console `battery init`).
esp_err_t Reinit();

// Console `battery` / hwinfo `[battery]`.
void PrintInfo();
// Console `battery regs`: 0x00..0x0F and the resident profile, raw.
void PrintRegs();

}  // namespace battery
