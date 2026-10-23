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
// `battery` command (boards with a fuel gauge / charger STAT line):
//   battery [info]   voltage, level, charging
//   battery regs     gauge registers 0x00..0x0F and the resident profile, raw
//   battery init     check the profile again (loads it if needed), reset the gauge
//
// `light` command (the frontlight; saved to config.json `frontlight`):
//   light [info]                  on/off, brightness, warmth, PWM duties
//   light <0..100> [warmth]       brightness (0: off), warmth 0 cool .. 100 warm
//   light warmth <0..100> | off
//
// `keys` command (the board's push buttons, keypad task):
//   keys [info]          per button: GPIO, raw and debounced level, held time; queue
//   keys watch [s]       events from now on for 1..120 s (default 10): pressed,
//                        released, long, click xN
//
// `led` command (boards with a WS2812):
//   led [info]                           pin, UART, colour, effect, ceilings, wire counters
//   led <colour> [effect] [seconds]      show it for a while (default 5 s), then back
//   led brightness [<n> [idle]]          ceilings in percent (config.json led; saved)

namespace console {

int CmdDate(int argc, char **argv);
int CmdBoard(int argc, char **argv);
int CmdLed(int argc, char **argv);
int CmdBattery(int argc, char **argv);
int CmdKeys(int argc, char **argv);
int CmdLight(int argc, char **argv);

// Readouts reused by `devstatus` and `hwinfo`.
void PrintDateInfo();
int PrintLedInfo();

}  // namespace console
