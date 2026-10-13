#pragma once

// `wifi` command (settings live in config.json; every setter saves it and
// applies it to the radio):
//   wifi on | off                radio on (in the configured mode) / off
//   wifi mode client|ap|apsta    set the mode (and switch the radio on)
//   wifi info                    mode, state, network, last error, MACs, IPv4, AP
//   wifi scan                    list access points (brings the radio up for it)
//   wifi ap [<ssid> [pw] [ch]]   show / set the device's own access point
//   wifi retry                   restart the current mode from the top
//   wifi networks                configured networks
//   wifi set <ssid> [password]   add/update a network (quote an SSID with spaces)
//   wifi static <n> <ip> <mask> <gw> [dns1] [dns2] | static <n> off
//                                fixed address for network n / back to DHCP
//   wifi forget <ssid> | all     remove a network / all
//
// `web` command (the configuration site):
//   web [info]                   wanted / running, address, auth, counters
//   web on | off | auto          when to serve (auto: while the device's AP is up)
//   web login <user> <pw> | off  basic auth for the whole site
//   web write on | off           allow / refuse settings changes from the site
//
// `config` command:
//   config show                  current values as JSON (passwords masked)
//   config info                  file path, source (file / defaults), last error
//   config reload                re-read config.json and apply Wi-Fi and web settings
//   config save                  write current values to config.json

namespace console {

int CmdWifi(int argc, char **argv);
int CmdWeb(int argc, char **argv);
int CmdConfig(int argc, char **argv);

// Readouts reused by `devstatus`.
void PrintWifiInfo();
void PrintWebInfo();

}  // namespace console
