#pragma once

// `server` command (components/server, config.json server.*):
//   server                    the host, the e-mail, registered or not, the
//                             link, the last sync, the public key
//   server check              https://<host>/ asked (connected or not)
//   server register [email]   a new key and TOTP secret, then the
//                             registration (the e-mail saved into config.json)
//   server code               the TOTP code for the server now
//   server host <name>        config.json server.host (saved)

namespace console {

int CmdServer(int argc, char **argv);

}  // namespace console
