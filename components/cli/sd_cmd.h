#pragma once

// `sd` command, the microSD card (read-only from here: it holds the reader's
// books too):
//   sd [info]      card, size, bus clock, mount point, FAT free/total; or why
//                  it is not mounted
//   sd ls [dir]    a directory: type, size, modification time (local), name

namespace console {

int CmdSd(int argc, char **argv);

}  // namespace console
