@echo off
rem LionKey's GoogleTest suite in WSL (gcc + ASan/UBSan): see lionkey.sh.
rem   host_test\lionkey.cmd            all tests
rem   host_test\lionkey.cmd <regex>    tests whose name matches, e.g. CtapGetInfo
rem Needs the Ubuntu WSL distribution and lib/lionkey/lib/tinyusb (submodule).
rem Kept ASCII and CRLF: cmd.exe misparses UTF-8 punctuation.
wsl -d Ubuntu --cd "%~dp0." -e bash ./lionkey.sh %*
