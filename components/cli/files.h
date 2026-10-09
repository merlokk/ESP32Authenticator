#pragma once

// `spiffs` command, files on the SPIFFS partition:
//   spiffs ls                  list files: size, modification time, totals
//   spiffs cat <file>          print a file as text
//   spiffs catbase64 <file>    print a file as base64, 76 chars per line
//   spiffs format confirm      erase the partition, create an empty fs

namespace console {

int CmdSpiffs(int argc, char **argv);

}  // namespace console
