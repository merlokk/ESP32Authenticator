#pragma once

// `fido2` command (the FIDO2 credential store):
//   fido2 [info]                 store use (bytes, credentials of max), PIN, counter
//   fido2 list                   credentials: #, id (first 8 hex), resident, site, user
//   fido2 delete <#|id>          delete one: number from `fido2 list` (1..3 digits) or a unique
//                                prefix of its id (4+ hex digits; numbers shift)
//   fido2 reset [confirm]        delete every credential and the PIN (asks first)

namespace console {

int CmdFido(int argc, char **argv);

// The `fido2 info` readout (used by `devstatus`).
void PrintFidoInfo();

}  // namespace console
