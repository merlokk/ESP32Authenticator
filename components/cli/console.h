#pragma once

// Device command line on the native USB Serial/JTAG port (esp_console REPL).
//
//   version    firmware version, build date, IDF version, chip, running slot
//   hwinfo     chip and IDs, flash, PSRAM, eFuse, NVS, partitions, temperature
//   efuse      every eFuse field and raw blocks
//   ls, cat, catbase64   SPIFFS files (see files.h)

#include "esp_err.h"

namespace console {

// Registers the commands and starts the REPL task.
esp_err_t Init();

}  // namespace console
