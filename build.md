# Build, flash, CLI

## ESP-IDF

v6.0.2, installed by `eim` (manifest: `C:\Espressif\tools\eim_idf.json`). Stay on
6.0.x: the stock X4 Pro firmware is built with 6.0.1.
`idf.py` is a PowerShell function that exists only after sourcing the profile:

```powershell
pwsh -NoProfile -Command "& { . 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1' *> `$null; idf.py build }"
```

## Build and flash

```powershell
idf.py set-target esp32s3   # once
idf.py build
idf.py -p COM6 flash
```

- Dev board: bare ESP32-S3 (8 MB octal PSRAM, 16 MB flash) on USB Serial/JTAG,
  used until the X4 Pro is available.
- A board running a TinyUSB app (PID 4001) ignores DTR/RTS. Enter download
  mode by hand: hold BOOT, press RESET.
- If the app does not start after flashing, run
  `python -m esptool -p COM6 --before no-reset --after watchdog-reset chip-id`.
- On the X4 Pro the app `.bin` is flashed through the CrossPoint unlocker-tool
  (see [architecture.md](architecture.md)).

## Version

`version.txt` + git → `PROJECT_VER`, e.g. `0.1.0-10-g060dff6[-dirty]`
(`cmake/version.cmake`). It is computed at configure time: run
`idf.py reconfigure` to pick up a new commit.

## CLI

`esp_console` REPL on USB Serial/JTAG (same port as the monitor), prompt `auth>`.
Opening the port with RTS asserted resets the chip.

| Command | Output |
|---|---|
| `help` | command list |
| `version` | firmware version, build date, IDF version, ELF SHA256, chip, running slot |
| `hwinfo` | chip, MACs, eFuse unique ID, flash JEDEC/size, PSRAM, heap, eFuse security/download/USB bits, key blocks, NVS stats, flash layout (bootloader version, partitions, image sizes, gaps), OTA state (running/boot/next slot, rollback, raw otadata), CPU temperature, reset reason, uptime |
| `efuse` | every eFuse field (list generated at build time from IDF `esp_efuse_table.csv`) and raw blocks BLK0..BLK10 |
