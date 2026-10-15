#!/usr/bin/env bash
# LionKey's own GoogleTest suite (lib/lionkey/test), built by gcc in WSL
# (MSVC cannot: C VLAs, sanitizers). Run from Windows via host_test\lionkey.cmd.
#   lionkey.sh            all tests
#   lionkey.sh <regex>    tests whose name matches (ctest -R), e.g. CtapGetInfo
# CMake/Ninja: ~/.local/opt (Kitware and ninja-build release binaries) or PATH.
# GoogleTest and {fmt} are fetched on the first configure (network).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../lib/lionkey"
# Build tree on the WSL file system: /mnt/e is slow.
build="${LIONKEY_BUILD_DIR:-$HOME/.cache/esp32auth-lionkey-build}"

opt="$HOME/.local/opt"
[ -x "$opt/cmake-3.31.6-linux-x86_64/bin/cmake" ] && PATH="$opt/cmake-3.31.6-linux-x86_64/bin:$PATH"
[ -x "$opt/ninja/ninja" ] && PATH="$opt/ninja:$PATH"
for tool in cmake ninja gcc g++; do
	command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

cmake -S "$src" -B "$build" -G Ninja -DBUILD_TARGET=host -DCMAKE_BUILD_TYPE=Debug \
	-DSANITIZERS=ON -DLIONKEY_DEBUG_LEVEL=1 >"$build.log" 2>&1 || { cat "$build.log"; exit 1; }
cmake --build "$build"
cd "$build"
if [ $# -gt 0 ]; then
	ctest --output-on-failure -j"$(nproc)" -R "$1"
else
	ctest --output-on-failure -j"$(nproc)"
fi
