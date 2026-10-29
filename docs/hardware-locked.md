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

## Stock OTA (X4 Pro)

From [unlocker-tool: X4 Pro OTA system](https://github.com/crosspoint-reader/crosspoint-tools/tree/master/unlocker-tool#x4-pro-ota-system)
and [X4Pro-002, section 7](https://github.com/jglerner/XTEink-Lab-X4-Pro/blob/main/docs/X4Pro-002%20-%20CrossPoint%20OTA%20Unlock%20Routine.md):

- Check: plain HTTP `GET http://api-prod.xteink.cc/api/v1/check-update`
  (`.cn` mirror), query `current_version`, `device_type`, `device_id`, `lng`,
  `ota_type`. The query's `device_type` carries the panel
  (`ESP32S3_X4_TL_SSD1677`), the header's is bare (`ESP32S3_X4_TL`); the bare
  one in the query: "No update available".
- Answer: `version`, `download_url` (Alibaba OSS, `firmware/encrypted/`),
  `ota_format: "encrypted_v1"`, `plain_size`, `plain_sha256`. Only `.xota`,
  never a plain image; `ota_type` 0 and 1: the same image under different AES
  keys (in `crates/unlocker-core/src/xota.rs`).
- `.xota`: metadata bytes 0-3 the body length (LE u32), 4-35 its SHA-256;
  the stock `60 a5 67 00` is no magic, it is 6,792,544, the stock image size.
- NTP before the check (`pool.ntp.org`, `time.google.com`): without an
  answer the device waits forever; the unlocker runs an SNTP responder.
- XTOS V7.6.10 asks AliDNS `223.5.5.5` directly, past the hosts file.
- A plain flash of a slot without `VALID` in otadata boots once, then rolls
  back ("had to flash twice"); the CRC covers only the sequence number.
- The unlock depends on the stock check-update behaviour: a later stock
  update may close it.

## USB on a locked unit

From [X4Pro-001](https://github.com/jglerner/XTEink-Lab-X4-Pro/blob/main/docs/X4Pro-001%20-%20Initial%20USB%20Connection%20Investigation.md)
(an AliExpress unit with China firmware, then CrossPoint):

- CrossPoint running normally: no enumeration (`error -71`, nothing in
  `lsusb`). Whether the Serial/JTAG ever shows is untested; esptool and the
  web flasher never connected.
- CrossPoint's USB Drive mode (TinyUSB MSC on the OTG controller):
  `303a:1001 CrossPoint CrossPoint_X4_Pro`. So `DIS_USB_OTG` is not burned
  there and this firmware's `serial` / `serial+fido` modes should work.
- The unlocker makes SD flashing possible, not USB flashing.

## Installing this firmware

From CrossPoint: `build/esp32-authenticator.bin` on the SD card, Settings >
System > SD Card Firmware Update. Without taking the card out:
[X4ProAirFlash](https://github.com/jglerner/XTEink-Lab-X4-Pro/blob/main/docs/X4Pro-003%20-%20X4ProAirFlash.md)
(`POST http://<device>/upload?path=/` to CrossPoint's web server, saved as
`/update.bin`), then the same menu; installing always takes a tap on the
device. The SD path compares no versions. Its checks
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
  With the Serial/JTAG off but OTG on (what the unit above looks like):
  Settings > Devices > USB port = serial (CDC-ACM). With `DIS_USB_OTG` the TinyUSB modes are refused
  (`ESP_ERR_NOT_SUPPORTED`, the port stays Serial/JTAG) instead of hanging in
  TinyUSB's core reset.
- Rollback: keep a CrossPoint `.bin` on the card; `/sdcard/firmware.bin`
  (button, `ota sd confirm`) installs it.
- Last resort: an SPI programmer on the external flash chip
  (`../crosspoint-reader/docs/fix-bricked-xteink.md`), only with flash
  encryption off.

## Sources

- [XTEink-Lab-X4-Pro](https://github.com/jglerner/XTEink-Lab-X4-Pro): X4 Pro research (USB, the OTA unlock, X4ProAirFlash).

- [unlocker-tool README](https://github.com/crosspoint-reader/crosspoint-tools/blob/master/unlocker-tool/README.md): `.xota`, flash layout, OTA state.
- [crosspoint-reader #1918](https://github.com/crosspoint-reader/crosspoint-reader/issues/1918): OTA on a USB-locked X4.
- [pocketink.io firmware FAQ](https://pocketink.io/firmware/faq/), [CrossPoint](https://pocketink.io/firmware/crosspoint/).
