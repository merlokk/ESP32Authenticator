# ESP32Authenticator

Hardware authenticator built on the **XTEINK X4 Pro Pocket eReader** (ESP32-S3).
Firmware: C/C++ on **ESP-IDF**.

## Overview

- Stores encrypted credential files (`vault.bin`, `fido.bin`); settings in
  `config.json`.
- Credential entry: site (name) / login / password / TOTP / FIDO2.
- Credentials are shown on the e-ink screen and sent to the host via:
  - **BLE keyboard** (HID): types login/password/TOTP code;
  - **FIDO2 / U2F** authenticator over BLE and USB HID.

## Status

- Done: BLE keyboard + FIDO2 (CTAP 2.3, works with Chrome on Windows 11),
  bonding and host switching; FIDO2 credential management on the console
  (`fido2 info|list|delete|reset`) and the store on the web settings page; Wi-Fi
  client / AP / AP+client with static IPs and an internet check; configuration
  web site; USB console; USB port modes switched at run time (`usb`:
  Serial/JTAG, TinyUSB serial, serial + FIDO2/U2F over HID, the SD card reader); `config.json`; time (SNTP, time zones, `rtc::Clock`:
  BM8563 RTC on the X4 Pro, else the CPU timer); board classes chosen in `devices/config.h`; WS2812 LED (red at boot,
  green breathing when ready, blue while typing, yellow
  blinking for the FIDO2 button); TOTP codes on the console (`totp`); hardware RNG (`devices/rng`, random hex
  on the console: `rng`); buttons (Left, Right, Power, Home on the
  GT911) in one keypad task: click xN / long events, `keys`; frontlight (cool/warm PWM,
  off at boot, Power x2 toggles, off when idle and on at a touch, `light`); e-ink panel driver (UC8179 /
  UC8279 / SSD1677 detected, full and fast refresh, `epd`; full after
  `display.fastRefreshes` fast or from `display.fullFromPercent` changed,
  dialogs appear in a full one, PIN dots go fast); battery: CW2017 gauge (voltage, level) and
  charger STAT on a leased I2C bus (`battery`, BLE Battery Level); microSD
  (SDMMC, FAT at `/sdcard`, `sd` read-only but wallpapers), the store files in
  `/sdcard/.authenticator` on the X4 Pro (no card: no stores); LVGL 9 on the e-ink panel (480x800 portrait, status bar, the password list in pages: a tap types
  the user / password / TOTP or shows the password; Settings (frontlight; Wi-Fi networks and
  the web site with its QR code and login; Bluetooth hosts and pairing;
  Devices: power, display, USB port, touch calibration; security: PIN
  change, wipe PIN, FIDO2 state and reset; date & time; About with
  Restart; dialogs in `dialog.h`) and the
  U2F / FIDO2 keys by swipes, deleted from their row; FIDO2 presence Left or Home yes / Right no; PIN pad while
the vault is locked, GT911 touch as the pointer, `ui`); power-off
  (Power held / x3 locked / idle `power.idleMinutes` / `off`): a random PNG from
  `/sdcard/wallpapers` (dithered to 4 grey levels on
  the UC8179 or B/W, `display.wallpaper`), deep
  sleep, Power wakes; the vault key kept in RTC memory through the sleep for
  `power.keepPinHours` (no PIN after the wake);
  password records (site/user/password/TOTP) in `vault.bin` with
  add/edit/delete on the console (`pass`) and the web site (with FIDO2
  credentials and TOTP codes); `vault.bin` and `fido.bin` are
  AES-256-GCM with a key = HKDF(seed phrase, PBKDF2(6-digit PIN)) (the
  same on synced devices; a PIN or seed change re-keys both; store header
  format 3 with a store id and a seed id), the seed phrase 12 BIP39 words in
  `/spiffs/seed.txt` (`seed`, the site's Passwords page: a new one from the
  hardware RNG), locked until `pin` (`test.pin` in config.json unlocks at
  boot). Authentication server (`server.host`, default `auth.merlokk.com`):
  Settings > Server (registered, connected by `/api/device/ping`, the clock
  against the server's, signed in, status sent, the TOTP code; Register: a
  new ECDSA P-256 key and TOTP secret in `/spiffs/server.bin`, an ES256 JWT
  POSTed to `https://<host>/register`, the `clientId` kept; refusals by their
  Problem Details `type`; sign-in by a client assertion to `/connect/token`;
  `POST /api/device/status` once Wi-Fi is up, then every
  `server.statusMinutes`, set on Server > Configure; `vault.bin` and
  `fido.bin` synced by their headers: on each change and every
  `server.syncMinutes`, a conflict or another PIN resolved on Server > Sync,
  the sync icon in the status bar), console `server`.
  PIN lockout (10 tries, `pin.lockMinutes`) and a wipe PIN (`pin wipe set`).
  LionKey fork (`merlokk/lionkey`, branch `esp32auth`): CTAP 2.3 (FIDO_2_3),
  U2F (BLE and USB), EdDSA, authenticatorConfig, minPinLength, PIN complexity,
  hmac-secret-mc, pcmr, attestation formats, encIdentifier, built-in UV
  plumbing (`uv: false`); see
  lionkey-update-plan.md. Firmware update (OTA): a `.bin` uploaded on the
  site (Device > Firmware update) or `/sdcard/firmware.bin` (button, `ota
  sd confirm`), checked while streaming (`image_check.h`), trial boot
  confirmed after 30 s, else rolled back.
- Not yet: eFuse-bound key; the sync of another seed phrase's stores.

## Architecture

Functions and status: [architecture.md](architecture.md#functionality); X4 Pro
screen design: [x4-design.md](x4-design.md).

Stack (ESP-IDF, BLE, Wi-Fi, web), config, tests, flash layout: [architecture.md](architecture.md).
Wi-Fi and the web site are ported from approver-esp32 (see [sources.md](sources.md)).

User manual with screenshots: [manual/README.md](manual/README.md).

## Build

ESP-IDF setup, build/flash, tests, web site access, CLI: [build.md](build.md).

Windows 11 BLE FIDO2 fix (GATT tail service): [windows-ble-fido.md](windows-ble-fido.md).

LionKey fork (CTAP 2.2/2.3 features, EdDSA, U2F): [lionkey-update-plan.md](lionkey-update-plan.md).

Credential store encryption, PSRAM store, SD for hundreds of keys (analysis): [storage-encryption.md](storage-encryption.md).

Testing on the board (console, screen screenshots and made-up touches,
`config test`, the FIDO2 suite): [testing-on-device.md](testing-on-device.md).

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
  hardware code only carries them out. Run `host_test\run.cmd` after changes
  (`host_test\lionkey.cmd` after changes in `lib/lionkey`).
- Nothing over ~1 KB on a task stack (main 3.5 KB, console/httpd 4 KB): use the
  heap or static storage (a stack copy of the config boot-looped the board).
- Passwords never in logs, console dumps or HTTP answers; the web write path is
  a whitelist; config changes from the site are memory-only until saved.
- Verify on the board after flashing (console via `utils/`, HTTP via `curl`).
- The board holds the user's real data (FIDO2 credentials, PIN, BLE bonds,
  Wi-Fi passwords): `fido2 reset`, deleting non-test credentials, `ble unpair`,
  `spiffs format` only when the user asks. Test leftovers (`ctap-test*.example`)
  may be removed.
