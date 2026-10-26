#pragma once

// `rng` command:
//   rng [length]   a random lowercase hex string of `length` digits (default
//                  64, up to rng::kMaxHexChars) from the hardware RNG; a
//                  second line warns when the radio is off (no true random)

namespace console {

int CmdRng(int argc, char **argv);

}  // namespace console
