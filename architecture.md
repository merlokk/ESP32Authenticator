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

## Code layout

| Path | Contents |
|---|---|
| `main/` | `app_main`: OTA self-confirm, NVS, SPIFFS, config, autostart of Wi-Fi/BLE, console |
| `components/` | app components (`cli`: USB command line; `config`: config.json; `wifimgr`: config to the Wi-Fi driver; `fido_ctap`: CTAP 2.1 core, see FIDO2) |
| `lib/` | third-party git submodules, used unmodified (`lionkey`) |
| `host_test/` | host tests (Unity, MSVC), no board: see Tests |
| `utils/` | host-side Python tools over the USB console (`spiffs.py`: ls/info/rm/get/put; `ble.py`: ble commands, `kb` text quoting) |
| `drivers/` | hardware drivers, one component per device (`hardware`: SoC info; `spiffs_fs`: SPIFFS on `spiffs` at `/spiffs`, never formatted, it holds stock data on the X4 Pro; `ble`, `ble_kb`, `ble_fido`: see BLE; `wifi`: station, tries the networks it is given in order, stores nothing; `buttons`: GPIO buttons, polled; `crypto`: hardware RNG, SHA-256, AES-256-CBC via PSA) |

## Config

`components/config`: `config.json` parsed into one fixed struct (cJSON).

- Search: `/spiffs/config.json` (TODO: microSD first). Missing, too big (> 4 KB)
  or invalid: built-in default config (`kDefaultJson`), reason in `config info`.
  Nothing is written at boot.
- Missing fields take defaults, unknown fields are ignored (lost on save), an
  invalid value rejects the whole file.
- Save is atomic: `config.json.new`, remove, rename; boot finishes an
  interrupted write.
- Plain text, Wi-Fi passwords included, until the encrypted storage exists.

```json
{
  "wifi": {
    "active": false,
    "hostname": "esp32-auth",
    "networks": [{"ssid": "Home", "password": "password123"}]
  },
  "ble": {"active": false}
}
```

`wifi.active` / `ble.active` start the radio at boot. Up to 4 networks, tried
in order. Password: empty (open), 8..63 chars, or 64 hex digits.

## Tests

`host_test/`: plain CMake project built by MSVC (ESP-IDF's `linux` target
cannot link on Windows), Unity from ESP-IDF, cJSON from `managed_components/`
(run `idf.py build` once). `fakes/` shadows the few ESP-IDF headers used.
Pattern from approver-esp32.

```powershell
host_test\run.cmd            # all suites
host_test\run.cmd config     # suites whose name contains "config"
```

## BLE

NimBLE, peripheral, one connection. Stack is off at boot (`ble on`).

| Driver | Role |
|---|---|
| `ble` | stack on/off, advertising (HID + FIDO UUIDs), bonding (NVS), DIS + Battery services; profile drivers register GATT services and GAP listeners before `ble on` |
| `ble_kb` | HID over GATT keyboard; types ASCII with a US layout |
| `ble_fido` | FIDO BLE transport (service 0xFFFD, CTAP 2.1 BLE framing, MTU fragmentation); CTAP is a pluggable handler run in a worker task (MSG meanwhile: ERR_BUSY; CANCEL and KEEPALIVE frames), default answers "not supported" |

Security:
- IO capability DisplayOnly: the device shows a 6-digit passkey, the host types it (MITM-protected, LE Secure Connections).
- New hosts pair only inside the pairing window (`ble pair`); other pairing attempts are rejected.
- One connection at a time. `ble use` picks the target bond: keys released on the old host, link dropped, advertising filtered (accept list) to the target; a non-target bonded host that still gets through is dropped after encryption.
- Unauthenticated (Just Works) links are dropped; HID reports and FIDO characteristics require an authenticated link.

## FIDO2

`components/fido_ctap`: [LionKey](https://github.com/pokusew/lionkey) CTAP 2.1
core (`lib/lionkey`, git submodule, not modified: update by moving the
submodule) behind `ble_fido`. API: `fido_ctap_init(hooks, store)`,
`fido_ctap_request(msg) -> response`.

- Crypto: lionkey's table with RNG, SHA-256, AES from the `crypto` driver;
  P-256 on lionkey's micro-ecc (no ECC accelerator on the S3; its `uECC_*`
  symbols are renamed `lk_uECC_*`, NimBLE's tinycrypt has the same names).
- Store: `/spiffs/fido.bin` = lionkey's RAM log (12 KB), rewritten whole
  through a temp file on every change, compacted at boot. Max 10 credentials:
  lionkey stores non-discoverable ones too, the 11th gets KEY_STORE_FULL.
  The signature counter is in NVS (`fido/sign_count`), kept by reset.
- User presence: a fresh press of the Left button (GPIO0, BOOT on the dev
  board) within 30 s; KEEPALIVE every 300 ms; CANCEL aborts the wait.
- Reset only within 10 s after boot (lionkey). No U2F/CTAP1.
- Tests: `tests/fido2` over BLE, all 67 pass on the dev board (see build.md).

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

- On the X4 Pro only the app `.bin` is replaced: it runs on the **stock
  bootloader** (ESP-IDF 6.0.1). Bootloader settings in our `sdkconfig` do not
  apply there, so the app must not depend on them.
- The stock bootloader has app rollback enabled. A new app must call
  `esp_ota_mark_app_valid_cancel_rollback()` after it boots, or the bootloader
  falls back to the old slot.
- Stock X4 Pro Wi-Fi OTA accepts only an encrypted `.xota` package
  (`encrypted_v1`): the image is AES-encrypted with a per-channel key and
  checked against `plain_sha256` after decryption. We do not build `.xota`
  ourselves: we ship a plain app `.bin`, and the CrossPoint unlocker-tool
  packages and flashes it.
- Source: [crosspoint-tools unlocker: Flash layout & OTA state](https://github.com/crosspoint-reader/crosspoint-tools/blob/master/unlocker-tool/README.md#flash-layout--ota-state), [FreeInk X4 Pro doc](https://github.com/Free-Ink/freeink-sdk/blob/main/docs/xteink-x4pro-support.md#partitions-16-mb-dual-ota).
