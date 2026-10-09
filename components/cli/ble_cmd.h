#pragma once

// `ble` command:
//   ble on | off               start / stop the BLE stack (off at boot)
//   ble info                   state, address, connection, pairing, bonds, profiles
//   ble pair [seconds]         open the pairing window (default 60 s) and wait:
//                              prints the passkey, returns on a new bond or
//                              when the window closes
//   ble pair <seconds> bg      open the window and return (passkey in the log)
//   ble pair stop              close it
//   ble unpair <addr> | all    delete a bond / all bonds
//   ble kb <text>              type text on the host (US layout; \n, \t escapes)

namespace console {

int CmdBle(int argc, char **argv);

}  // namespace console
