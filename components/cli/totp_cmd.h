#pragma once

// `totp` command:
//   totp <base32 secret>   the time and the codes of the previous, current and
//                          next 30 s windows (6 digits, HMAC-SHA1); an error
//                          without a believable time. The secret is never
//                          printed or logged (it stays in the up-arrow history)
//   totp test              the RFC 6238 vectors through the crypto driver

namespace console {

int CmdTotp(int argc, char **argv);

}  // namespace console
