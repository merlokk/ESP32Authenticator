#pragma once

// `wifi` command (station mode):
//   wifi on | off              start (connects to the saved network) / stop
//   wifi info                  state, last error, MAC, network, RSSI, IPv4
//   wifi scan                  list access points (radio must be on)
//   wifi set <ssid> [password] save the network (quote an SSID with spaces)
//   wifi forget                delete the saved network

namespace console {

int CmdWifi(int argc, char **argv);

}  // namespace console
