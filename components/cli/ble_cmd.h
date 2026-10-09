#pragma once

// `ble` command:
//   ble on | off               start / stop the BLE stack (off at boot)
//   ble info                   state, address, connection, pairing, bonds, profiles
//   ble pair [seconds]         open the pairing window (default 60 s) and wait:
//                              prints the passkey, returns on a new bond or
//                              when the window closes
//   ble pair <seconds> bg      open the window and return (passkey in the log)
//   ble pair stop              close it
//   ble bonds                  numbered bond list, marks the connected and target host
//   ble conns                  the connection: peer, security, MTU, params, RSSI
//   ble use <#|addr|any>       switch hosts: target one bond (keys released, link
//                              dropped, only the target may reconnect) or any
//   ble disconnect             drop the link (keys released first)
//   ble unpair <#|addr|all>    delete a bond (number from `ble bonds` or address)
//                              or all bonds; a connected host is disconnected
//   ble kb <text>              type text on the host (US layout; \n, \t escapes)

namespace console {

int CmdBle(int argc, char **argv);

}  // namespace console
