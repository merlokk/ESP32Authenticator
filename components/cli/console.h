#pragma once

// Device command line on the native USB Serial/JTAG port (esp_console REPL).
//
//   term       line editing / up-arrow history: smart, dumb, or probe
//   version    firmware version, build date, IDF version, chip, running slot
//   info       short hardware summary
//   hwinfo     chip and IDs, flash, PSRAM, eFuse, NVS, partitions, temperature
//   efuse      every eFuse field and raw blocks
//   spiffs     info | ls | cat | catbase64 | write | rm | format (see files.h)
//   ble        on | off | info | pair | bonds | conns | use | unpair | kb (see ble_cmd.h)
//   wifi       on | off | info | scan | networks | set | forget (see wifi_cmd.h)
//   config     show | info | reload | save (see wifi_cmd.h)

#include "esp_err.h"

namespace console {

// Registers the commands and starts the REPL task.
esp_err_t Init();

}  // namespace console
