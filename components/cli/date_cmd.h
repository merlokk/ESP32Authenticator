#pragma once

// `date` command (settings in config.json `time`; setters save it):
//   date [info]                       UTC and local time, zone, clock, sync state
//   date sync                         ask the time server now and wait for it
//   date set <YYYY-MM-DD> <HH:MM:SS>  set the clock, local time
//   date set utc <YYYY-MM-DD> <HH:MM:SS>
//   date zone [<name|POSIX rule>]     show / set the time zone
//   date zones [filter]               the zone table
//   date server <host|off>            SNTP server (off: no sync)
//   date hours <0..168>               sync interval (0: no sync)
//
// `board` command: the board this firmware is built for (devices/config.h),
// its devices and pins.
//
// `led` command (boards with a WS2812):
//   led                                  colour, effect, ceilings, wire counters
//   led <colour> [effect] [seconds]      show it for a while (default 5 s), then back

namespace console {

int CmdDate(int argc, char **argv);
int CmdBoard(int argc, char **argv);
int CmdLed(int argc, char **argv);

// Readout reused by `devstatus`.
void PrintDateInfo();

}  // namespace console
