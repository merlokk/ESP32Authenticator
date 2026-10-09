#pragma once

#include "esp_err.h"

// SoC-level hardware info: chip and IDs, flash, PSRAM, eFuse security state,
// NVS, partitions, CPU temperature, reset reason, heap.

namespace hardware {

// Prints the full report to stdout (used by the `hwinfo` command).
void PrintInfo();

// Prints every named eFuse field (list generated from the IDF eFuse table).
void PrintEfuseFields();

// Prints raw eFuse block registers BLK0..BLK10.
void PrintEfuseRaw();

// Reads the on-die temperature sensor.
esp_err_t CpuTemperature(float *celsius);

}  // namespace hardware
