# X4 Pro stock firmware backup

Read-only backup of a new XTEINK X4 Pro taken on 2026-10-23 (esptool/espefuse
v5.3.1, COM7, USB-Serial/JTAG). Nothing was written to flash or eFuses.

## Device

| Item | Value |
|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2, dual core 240 MHz, 40 MHz XTAL |
| PSRAM | 8 MB embedded, AP_3v3, 85 °C |
| Flash | 16 MB, JEDEC 0x85/0x2018 (Puya 128 Mbit), quad, 3.3 V, image header DIO 80 MHz |
| MAC | c4:9e:7e:29:22:e8 |
| eFuse BLOCK3 (user data) | `X4CB02EN26092601366` (factory serial) |
| Security | secure boot off, flash encryption off, key blocks empty, JTAG/USB/download mode enabled, no eFuse write/read protection |
| USB console | stock firmware prints nothing on USB-Serial/JTAG after boot |

## Stock firmware

| Item | Value |
|---|---|
| Bootloader | ESP-IDF v6.0.1, built Sep 15 2026 14:50:48 |
| App | `xteink_app` 7.5.11, ESP-IDF v6.0.1, built Sep 15 2026 14:51:03 |
| App ELF SHA256 | 5ee64a2e82028e15cfb0a629d1c31e55a8d9b806830cb764e3e69f6ca1f765ab |
| Running slot | app0 (otadata seq 1, state VALID); app1 empty |
| Cloud | `api-prod.xteink.cc/api/v1`, `api-prod.xteink.cn/api/v1` (OTA, sync) |
| Drivers seen in strings | UC8179/UC8279 EPD (SPI 3-wire+DC, GDMA), BM8563 RTC, GT911 touch (disabled on this board), CW2017 fuel gauge, BLE keyboard/pager/gamepad, USB MSC |
| IDs in strings | FCC ID 2BTR9-X4PRO, IC 35766-X4PRO |

NVS (`hw_calib`, `user_config`): screenType=1, screenId=1, region=2,
language=1, timeZone=-5, pwrTimingVer=2, Wi-Fi SSID/password empty, plus PHY
calibration. SPIFFS holds only `/README.md` (default placeholder).

## Partition table (0x8000)

| Name | Type | SubType | Offset | Size | Used |
|---|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 20K | 1 page |
| otadata | data | ota | 0xe000 | 8K | yes |
| app0 | app | ota_0 | 0x10000 | 8064K | 5.2 MB |
| app1 | app | ota_1 | 0x7f0000 | 8064K | empty |
| spiffs | data | spiffs | 0xfd0000 | 80K | yes |
| coredump | data | coredump | 0xfe4000 | 112K | empty |

## Files

- `x4pro_full_16MB.bin`: whole flash, MD5 075b861e8638943a54372132875ca19c,
  confirmed by `esptool verify-flash` (on-chip digest).
- `partition_table.bin` / `.csv`: 0x8000..0x9000 and its decoding.
- `parts/`: bootloader, nvs, otadata, app0 (trimmed to used size), spiffs;
  file names carry the flash offset. Empty app1/coredump are not stored.
- `efuse_summary.txt`, `efuse_dump.txt`: all eFuse fields and raw blocks.
- `SHA256SUMS`.

## Restore

Back to the stock firmware (from the repo root, inside the IDF profile):

```
python -m esptool -p COM7 write-flash 0 originalfw/x4pro_full_16MB.bin
```

The `.bin` files are not in git (`.gitignore`): keep a copy of this folder
outside the repo. This restores NVS too (calibration, settings); the factory state needs no
other files. eFuses are untouched by any of this.

## Dump notes

The stub flasher drops the link ("No more data to read") on some 64 KB blocks
over USB-Serial/JTAG at any baud; those 10 blocks were read with `--no-stub`
(ROM, ~150 kbit/s), the rest with the stub, then the whole image was checked
with `verify-flash`.
