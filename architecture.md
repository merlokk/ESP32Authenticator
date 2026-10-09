# Architecture

## Stack

| Layer | Choice |
|---|---|
| Framework | ESP-IDF (C/C++) |
| UI | LVGL, rendered to the e-ink panel |
| Host link | BLE HID keyboard (types credentials), BLE FIDO2 (CTAP2) |
| Network | Wi-Fi: syncing data and configuring the device |
| Storage | encrypted file with settings and credentials |

Reference ESP-IDF + LVGL project: `../../ai-remote/approver-esp32` ([GitHub](https://github.com/merlokk/ai-remote/tree/main/approver-esp32)).

## Flash layout

The firmware `.bin` must be compatible with the stock reader firmware: some
readers have the USB flasher disabled, so they are flashed through the reader's
own OTA path. Keep the stock X4 Pro partition table, 16 MB, dual OTA:

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x009000 | 0x005000 |
| otadata | data/ota | 0x00E000 | 0x002000 |
| app0 | app/ota_0 | 0x010000 | 0x7E0000 |
| app1 | app/ota_1 | 0x7F0000 | 0x7E0000 |
| spiffs | data/spiffs | 0xFD0000 | 0x014000 |
| coredump | data/coredump | 0xFE4000 | 0x01C000 |

- The stock bootloader has app rollback enabled. A new app must call
  `esp_ota_mark_app_valid_cancel_rollback()` after it boots, or the bootloader
  falls back to the old slot.
- Source: [crosspoint-tools unlocker: Flash layout & OTA state](https://github.com/crosspoint-reader/crosspoint-tools/blob/master/unlocker-tool/README.md#flash-layout--ota-state), [FreeInk X4 Pro doc](https://github.com/Free-Ink/freeink-sdk/blob/main/docs/xteink-x4pro-support.md#partitions-16-mb-dual-ota).
