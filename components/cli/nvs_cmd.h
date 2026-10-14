#pragma once

// `nvs` command (development): the default NVS partition's keys.
//   nvs ls [namespace]        namespace, key, type; never the values (BLE
//                             bond keys, counters)
//   nvs rm <namespace> <key>  erase one key

namespace console {

int CmdNvs(int argc, char **argv);

}  // namespace console
