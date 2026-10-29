#pragma once

// `nvs` command (development): the default NVS partition's keys.
//   nvs ls [namespace]        namespace, key, type, value: numbers, strings
//                             (a password, credential or key: its length
//                             only, nvs_show.h), blobs by size
//   nvs rm <namespace> <key>  erase one key

namespace console {

int CmdNvs(int argc, char **argv);

}  // namespace console
