# ESP32Authenticator

Hardware authenticator built on the **XTEINK X4 Pro Pocket eReader** (ESP32-S3).
Firmware: C/C++ on **ESP-IDF**.

## Overview

- Stores an encrypted file with device settings and credentials.
- Credential entry: site (name) / login / password / TOTP / FIDO2.
- Credentials are shown on the e-ink screen and sent to the host via:
  - **BLE keyboard** (HID): types login/password/TOTP code;
  - **BLE FIDO2** authenticator.

## Status

- Done: BLE keyboard + FIDO2 (CTAP 2.1, works with Chrome on Windows 11),
  bonding and host switching; FIDO2 credential management on the console
  (`fido2 info|list|delete|reset`) and the store on the web front page; Wi-Fi
  client / AP / AP+client with static IPs and an internet check; configuration
  web site; USB console; `config.json`; time (SNTP, time zones, `rtc::Clock`
  on the CPU timer); board classes chosen in `devices/config.h`; WS2812 LED (red at boot,
  green breathing when ready); TOTP codes on the console (`totp`); LVGL 9 library (no display driver).
- Not yet: BM8563 RTC driver, e-ink UI (LVGL), encrypted storage, credential
  records (site/login/password/TOTP), sync over Wi-Fi, microSD config.

## Architecture

Functions and status: [architecture.md](architecture.md#functionality); X4 Pro
screen design: [x4-design.md](x4-design.md).

Stack (ESP-IDF, BLE, Wi-Fi, web), config, tests, flash layout: [architecture.md](architecture.md).
Wi-Fi and the web site are ported from approver-esp32 (see [sources.md](sources.md)).

## Build

ESP-IDF setup, build/flash, tests, web site access, CLI: [build.md](build.md).

Windows 11 BLE FIDO2 fix (GATT tail service): [windows-ble-fido.md](windows-ble-fido.md).

Credential store encryption, PSRAM store, SD for hundreds of keys (analysis): [storage-encryption.md](storage-encryption.md).

## Hardware

Pinout, display controllers, devices: [hardware.md](hardware.md).

## Sources

Reference projects (local paths + git): [sources.md](sources.md).

## Rules

- All docs and code comments are in English.
- Docs are as compact as possible.
- Commit directly to `main` and push right after committing.
- Commits and MRs: append ` +ai` to the subject/title; no `Co-Authored-By` or similar trailers.
- Reply in the language of the request.
- Decisions live in pure headers (`<cstdint>`/`<cstddef>` only) with host tests;
  hardware code only carries them out. Run `host_test\run.cmd` after changes.
- Nothing over ~1 KB on a task stack (main 3.5 KB, console/httpd 4 KB): use the
  heap or static storage (a stack copy of the config boot-looped the board).
- Passwords never in logs, console dumps or HTTP answers; the web write path is
  a whitelist; config changes from the site are memory-only until saved.
- Verify on the board after flashing (console via `utils/`, HTTP via `curl`).
- The board holds the user's real data (FIDO2 credentials, PIN, BLE bonds,
  Wi-Fi passwords): `fido2 reset`, deleting non-test credentials, `ble unpair`,
  `spiffs format` only when the user asks. Test leftovers (`ctap-test*.example`)
  may be removed.
