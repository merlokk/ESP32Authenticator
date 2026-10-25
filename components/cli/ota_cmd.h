#pragma once

// `ota` command, firmware updates (ota_update.h):
//   ota [info]          running slot, version, state (a trial boot: seconds to
//                       its confirmation), next slot, the last job, the image
//                       refused at boot, /sdcard/firmware.bin and its check
//   ota sd [confirm]    the check of /sdcard/firmware.bin / writes it into the
//                       next slot and restarts

namespace console {

int CmdOta(int argc, char **argv);

}  // namespace console
