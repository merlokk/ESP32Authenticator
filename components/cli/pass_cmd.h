#pragma once

// `pass` command (the password store, vault.h). Passwords are never printed.
//   pass [info]                         state, file, records of max, version
//   pass list [filter]                  #, site, user, password set, TOTP
//   pass add <site> [user] [password] [otp]
//   pass set <#> <site|user|password|otp> [value]   no value: clear
//   pass delete <#>
//   pass code <#>                       TOTP codes of a record
//   pass reset [confirm [pin]]          delete every record; locked or refused:
//                                       `pin` is the new store's PIN
// Numbers are list positions (sorted by site, user): they shift on changes.
// Quote values with spaces: pass add "My Bank" "john doe".

namespace console {

int CmdPass(int argc, char **argv);

// `pin` command: the vault PIN.
//   pin [info]                 locked or open, key derivation
//   pin <6 digits>             unlock (no store yet: makes one with this PIN)
//   pin change <old> <new>
//   pin lock                   wipe the key and the records from memory
//   pin wipe set <pin> | off   the wipe PIN (config.json pin.wipe: salted hash)
int CmdPin(int argc, char **argv);

// `seed` command: the seed phrase (vault.h, seed_phrase.h). The words are
// printed only by `show` and `new`, asked for (to write them down).
//   seed [info]                  set or not, its id and the one vault.bin names
//   seed show <pin>              the words (the store open)
//   seed set [<pin>] <words>     locked: the phrase to open the stores with;
//                                open: a new key with it (the PIN needed)
//   seed new [<pin>]             a random phrase (hardware RNG), as set
int CmdSeed(int argc, char **argv);

// The `pass info` readout (used by `devstatus`).
void PrintPassInfo();

}  // namespace console
