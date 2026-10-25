#pragma once

// `sd` command, the microSD card (read-only from here, wallpapers aside: it
// holds the store files and the reader's books):
//   sd [info]      card, size, bus clock, mount point, FAT free/total; or why
//                  it is not mounted
//   sd ls [dir]    a directory: type, size, modification time (local), name
//   sd put <name.png> <length> <crc32>
//                  a wallpaper into /sdcard/wallpapers: one base64 line, as
//                  `spiffs write` (utils/wallpaper.py sends it)
//   sd rm <name.png>   deletes a wallpaper
//   sd put|rm firmware.bin   /sdcard/firmware.bin for `ota sd` (utils/firmware.py)

namespace console {

int CmdSd(int argc, char **argv);

}  // namespace console
