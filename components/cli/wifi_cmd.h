#pragma once

// `wifi` command (station mode; networks live in config.json):
//   wifi on | off                start (tries the configured networks) / stop
//   wifi info                    state, network, last error, MAC, RSSI, IPv4
//   wifi scan                    list access points (radio must be on)
//   wifi networks                configured networks
//   wifi set <ssid> [password]   add/update a network and save config.json
//                                (quote an SSID with spaces)
//   wifi forget <ssid> | all     remove a network / all and save
//
// `config` command:
//   config show                  current values as JSON (passwords masked)
//   config info                  file path, source (file / defaults), last error
//   config reload                re-read config.json and apply Wi-Fi settings
//   config save                  write current values to config.json

namespace console {

int CmdWifi(int argc, char **argv);
int CmdConfig(int argc, char **argv);

}  // namespace console
