#pragma once

// `pass` command (the password store, vault.h). Passwords are never printed.
//   pass [info]                         state, file, records of max, version
//   pass list [filter]                  #, site, user, password set, TOTP
//   pass add <site> [user] [password] [otp]
//   pass set <#> <site|user|password|otp> [value]   no value: clear
//   pass delete <#>
//   pass code <#>                       TOTP codes of a record
//   pass reset [confirm]                delete every record (also a refused file)
// Numbers are list positions (sorted by site, user): they shift on changes.
// Quote values with spaces: pass add "My Bank" "john doe".

namespace console {

int CmdPass(int argc, char **argv);

// The `pass info` readout (used by `devstatus`).
void PrintPassInfo();

}  // namespace console
