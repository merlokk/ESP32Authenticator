# USB-locked X4 Pro

Some X4 Pro units (Chinese retail, `device_type=ESP32S3_X4_TL_SSD1677`) ship
with flashing over USB-C off: esptool does not connect, eFuses cannot be read
from the PC. Which bits are burned is not documented anywhere.

## What the lock is

- Stock OTA takes only an `encrypted_v1` `.xota` package (`XOTA`, IV,
  AES-128-CTR metadata + body, SHA-256 of the body). This is the transport
  encryption, not eFuse flash encryption. The Xteink Unlocker
  ([unlocker-tool](https://github.com/crosspoint-reader/crosspoint-tools/blob/master/unlocker-tool/README.md))
  wraps CrossPoint into it and serves it from a spoofed update server.
- Secure boot: off in practice (CrossPoint, not signed by Xteink, boots).
- Flash encryption (eFuse): unknown, and no firmware can turn it off. It does
  not matter for OTA: `esp_partition_write` / `esp_ota_write` encrypt
  themselves on an encrypted chip.

## Installing this firmware

From CrossPoint: `build/esp32-authenticator.bin` on the SD card, Settings >
SD Card Firmware Update. Its checks
(`../crosspoint-reader/src/network/FirmwareFlasher.cpp`) all pass:

| Check | This image |
|---|---|
| chip id | ESP32-S3 |
| header, segments, XOR checksum, SHA-256 trailer | as `idf.py build` writes them |
| size <= slot (0x7E0000) | ~2.5 MB |
| board tag `CROSSPOINT-BOARD-V1:<name>;` | only checked if present; none here |

The partition table stays stock and matches `partitions.csv`. The image goes
to the other slot with otadata state `NEW`: `ota::Boot` confirms it after
30 s; a reset before that rolls back to CrossPoint (a hang does not).

## After the first boot

- eFuses: `hwinfo.html` on the web site (or `hwinfo` on the console): secure
  boot, flash encryption, `DIS_USB_*`.
- Console: with only the download mode off, the Serial/JTAG console works.
  With the Serial/JTAG off but OTG on: Settings > Devices > USB port = serial
  (CDC-ACM). With `DIS_USB_OTG` the TinyUSB modes are refused
  (`ESP_ERR_NOT_SUPPORTED`, the port stays Serial/JTAG) instead of hanging in
  TinyUSB's core reset.
- Rollback: keep a CrossPoint `.bin` on the card; `/sdcard/firmware.bin`
  (button, `ota sd confirm`) installs it.
- Last resort: an SPI programmer on the external flash chip
  (`../crosspoint-reader/docs/fix-bricked-xteink.md`), only with flash
  encryption off.

## Sources

- [unlocker-tool README](https://github.com/crosspoint-reader/crosspoint-tools/blob/master/unlocker-tool/README.md): `.xota`, flash layout, OTA state.
- [crosspoint-reader #1918](https://github.com/crosspoint-reader/crosspoint-reader/issues/1918): OTA on a USB-locked X4.
- [pocketink.io firmware FAQ](https://pocketink.io/firmware/faq/), [CrossPoint](https://pocketink.io/firmware/crosspoint/).
