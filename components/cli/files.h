#pragma once

// File commands on the SPIFFS partition:
//   ls                 list files: size, modification time, totals
//   cat <file>         print a file as text
//   catbase64 <file>   print a file as base64, 76 chars per line

namespace console {

int CmdLs(int argc, char **argv);
int CmdCat(int argc, char **argv);
int CmdCatBase64(int argc, char **argv);

}  // namespace console
