# Sources

Reference projects. Local paths are relative to the repo root.

| Project | Local | Git | Use |
|---|---|---|---|
| Solo 1 | `../solo1` | https://github.com/solokeys/solo1 | Open-source FIDO2 security key: CTAP2/FIDO2 implementation. |
| CrossPoint Reader | `../crosspoint-reader` | https://github.com/crosspoint-reader/crosspoint-reader | Open-source e-reader firmware for XTEINK X4: device support, UI. |
| FreeInk SDK | `../crosspoint-reader/freeink-sdk` | https://github.com/Free-Ink/freeink-sdk | Hardware-independent e-paper reader SDK (CrossPoint submodule). |
| FreeInk X4 Pro doc | `../crosspoint-reader/freeink-sdk/docs/xteink-x4pro-support.md` | https://github.com/Free-Ink/freeink-sdk/blob/main/docs/xteink-x4pro-support.md | X4 Pro hardware RE, confirmed on hardware: pinout, display variants, touch, SD, gauge, partitions. |
| TuyaOpen XTEINK_X4_PRO | — | https://github.com/tuya/TuyaOpen/tree/master/boards/ESP32/XTEINK_X4_PRO | Board support for XTEINK X4 Pro (ESP32-S3): pins, peripherals. |
| CrossPoint unlocker-tool | — | https://github.com/crosspoint-reader/crosspoint-tools/blob/master/unlocker-tool/README.md#flash-layout--ota-state | XTEINK X4 flash layout (partitions) and OTA state. |
| ESP32Auth | — | https://github.com/martin-ger/ESP32Auth | NimBLE FIDO BLE service skeleton (GATT layout, framing); CTAP via solo. Base for `ble_fido`. |
| CTAP 2.1 spec | — | https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html | BLE transport (11.4), CTAP2 commands. |
| approver-esp32 | `../../ai-remote/approver-esp32` | https://github.com/merlokk/ai-remote/tree/main/approver-esp32 | Reference ESP-IDF + LVGL device project (own repo). |
