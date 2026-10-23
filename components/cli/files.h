#pragma once

#include <cstddef>
#include <cstdint>

// `spiffs` command, files on the SPIFFS partition:
//   spiffs info                partition, mount point, total/used/free, file count
//   spiffs ls                  list files: size, modification time, totals
//   spiffs cat <file>          print a file as text
//   spiffs catbase64 <file>    print a file as base64, 76 chars per line
//   spiffs write <file> [<length> <crc32>]
//                              receive one base64 line (Enter ends the file),
//                              decode it into <file>, check length/crc32
//   spiffs rm <file>           delete a file
//   spiffs format confirm      erase the partition, create an empty fs

namespace console {

int CmdSpiffs(int argc, char **argv);

// Receives one base64 line from the console (flow control: a '.' every 192
// chars) into `path`; with `check`, length and CRC32 must match, else the
// file is removed. Prints "ok <n> bytes, crc32 <x>" or "error: ...".
int ReceiveFile(const char *path, size_t want_length, uint32_t want_crc, bool check);

}  // namespace console
