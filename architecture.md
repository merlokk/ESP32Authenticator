# Architecture

## Stack

| Layer | Choice |
|---|---|
| Framework | ESP-IDF (C/C++) |
| UI | LVGL 9 (`components/ui`: library up, no display driver yet; today: USB console + web site) |
| Host link | BLE HID keyboard (types credentials), BLE FIDO2 (CTAP2) |
| Network | Wi-Fi (client / AP / AP+client) and a configuration web site |
| Storage | `config.json` and `fido.bin` on SPIFFS, plain (encrypted storage planned) |

Reference ESP-IDF + LVGL project: `../../ai-remote/approver-esp32` ([GitHub](https://github.com/merlokk/ai-remote/tree/main/approver-esp32)).

## Code layout

| Path | Contents |
|---|---|
| `main/` | `app_main`: OTA self-confirm, NVS, SPIFFS, config, console, Wi-Fi manager + web server, BLE autostart |
| `components/` | app components (`cli`: USB command line; `config`: config.json; `wifimgr`: Wi-Fi policy + manager; `web`: configuration site, see Wi-Fi and web; `fido_ctap`: CTAP 2.1 core, see FIDO2; `timesync`, `timezone`: see Time; `totp`: Base32 secrets and the previous/current/next windows, codes from `crypto::Totp`; `ui`: LVGL, see UI) |
| `lib/` | third-party git submodules, used unmodified (`lionkey`) |
| `host_test/` | host tests (Unity, MSVC), no board: see Tests |
| `utils/` | host-side Python tools over the USB console (`device.py`: console session; `spiffs.py`: ls/info/rm/get/put; `ble.py`: ble commands, `kb` text quoting) |
| `devices/` | hardware drivers, one component per device (`hardware`: SoC info, eFuse, USB host connected; `spiffs_fs`: SPIFFS on `spiffs` at `/spiffs`, never formatted, it holds stock data on the X4 Pro; `ble`, `ble_kb`, `ble_fido`: see BLE; `wifi`: the radio (`wifi::Radio`), no decisions, stores nothing; `buttons`: GPIO buttons, polled, pins from the board; `crypto`: hardware RNG, SHA-256, AES-256-CBC, HMAC-SHA1 via PSA, HOTP/TOTP (`otp.h`, pure: counter, truncation); `clock`: `rtc::Clock`, see Time; `led`: one WS2812 from UART1, see Boards; `boards`: board classes, see Boards). `devices/config.h`: the board the firmware is built for |

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
- `Data` is over a kilobyte: parsing stages it on the heap, never on a task
  stack (main 3.5 KB, console and httpd 4 KB; a stack copy overflowed main
  and boot-looped the board).

```json
{
  "wifi": {
    "active": false,
    "mode": "client",
    "hostname": "esp32-auth",
    "rounds": 2,
    "apWindowSeconds": 120,
    "ap": {"ssid": "esp32-auth", "password": "esp32-auth", "channel": 6},
    "networks": [
      {"ssid": "Home", "password": "password123"},
      {"ssid": "Office", "password": "", "ip": {"static": true, "address": "10.0.0.5",
        "netmask": "255.255.255.0", "gateway": "10.0.0.1", "dns1": "10.0.0.1", "dns2": ""}}
    ]
  },
  "internet": {"check": true, "intervalSeconds": 60, "timeoutMs": 2000, "failures": 2,
               "targets": ["8.8.8.8", "1.1.1.1", "9.9.9.9"]},
  "ble": {"active": false},
  "web": {"mode": "auto", "write": true, "user": "", "password": ""},
  "time": {"zone": "UTC", "server": "pool.ntp.org", "syncHours": 6},
  "led": {"brightness": 15, "idleBrightness": 7}
}
```

`wifi.active` / `ble.active` start the radio at boot. Up to 4 networks, tried
in order. Password: empty (open), 8..63 chars, or 64 hex digits. AP password:
empty (open) or 8..63 chars; `rounds` 1..10, `apWindowSeconds` 30..3600,
channel 1..13. Change the default AP password. `ip` (optional, per network):
`static` false or absent is DHCP (the typed fields are kept); with `static`
true address/netmask/gateway are required, DNS optional; dotted quads, no
leading zeros. `internet`: `intervalSeconds` 5..3600, `timeoutMs` 100..10000,
`failures` 1..10, up to 4 IPv4 `targets` (pinged: no names). `time`: `zone` a
table name (`date zones`) or a POSIX TZ rule; `server` a host name or IPv4,
empty: no sync; `syncHours` 0..168, 0: no sync. `led`: brightness ceilings in
percent, 0..100 (`idleBrightness`: resting states); colours are compiled in.

## Tests

`host_test/`: plain CMake project built by MSVC (ESP-IDF's `linux` target
cannot link on Windows), Unity from ESP-IDF, cJSON from `managed_components/`
(run `idf.py build` once). `fakes/` shadows the few ESP-IDF headers used.
Pattern from approver-esp32. Suites: `config`, `wifi_policy`, `reachability`,
`web_auth`, `web_paths` (paths + server policy), `web_settings`, `clock`,
`boards`, `timesync`, `timezone`, `led`, `totp` (truncation against the RFC 4226
HMAC values; the HMAC itself is checked on the board by `totp test`); only code with
no hardware (pure headers) is under test.

```powershell
host_test\run.cmd            # all suites
host_test\run.cmd config     # suites whose name contains "config"
```

## Boards

`devices/boards`: one class per board, deriving from `boards::Board`
(`board_spec.h`, pure): `hw_clock` (none, BM8563), `display` (none, e-ink
800x480), `touch` (none, GT911), `led` (none, WS2812), `battery` (none,
CW2017), `has_sd_card`, `has_frontlight`, and GPIOs for the buttons (Left,
Right, Power; Left is FIDO2 user presence) and every device. Chosen at build
time: `kBoardId` in `devices/config.h` -> `boards::kBoard` (`board.h`,
constexpr); `CheckPins` (no shared GPIO, no USB/flash/PSRAM pin, every device
has its pins) is a `static_assert` and a host test.

| Class | Board |
|---|---|
| `DevBoard` (`dev_board.h`) | bare ESP32-S3 dev board (YD-ESP32-S3 type): BOOT = Left on GPIO0, WS2812 on GPIO48, no RTC |
| `X4Pro` (`x4_pro.h`) | XTEINK X4 Pro, pins from [hardware.md](hardware.md) |

`board_devices.h` carries it out: `boards::Init()` (the clock), `Clock()`,
`Button(id)`, `PrintInfo()` (`board`), `BootDone()`. No display, touch or SD
driver yet.

LED (`devices/led`, from approver-esp32-yubikey): WS2812 driven by UART1 at
3.33 Mbaud, 6 data bits, TX inverted (two bit cells per character, no RMT);
a task shows what it was last told (`Animator` in `led_frames.h`, pure: solid,
beacon, blinks, a 9 s breath linear in perceived lightness; temporary
overrides). Ceilings from `config.json` `led` (default 15 % / 7 % idle), set
right after the config is read. Boot: red solid from the first
line of `app_main` (dark = hardware fault, red = firmware running), green
breathing at the idle ceiling when `app_main` is done.

## Time

The clock is UTC; a zone is presentation only.

| Part | Role |
|---|---|
| `devices/clock` | `rtc::Clock` interface: carries UTC to the next boot (`Read`/`Write`, `KeepsTimeWhenOff`). `TimerClock`: the chip's RTC timer + state in RTC no-init memory, kept by software resets, lost at power-off. `clock_math.h` (pure): date <-> Unix, believable range 2024..2099, the timer state (magic + FNV check) |
| `components/timesync` | SNTP (`esp_netif_sntp`), one task, 1 s tick. `sync_policy.h` (pure, from approver-esp32): sync at boot / when the internet appears / every `syncHours`; no re-sync within 5 min of a good one (flapping link); failure retry 1 min doubling to 15 min. Internet = a client link with an address not reported offline by the check. An answer outside 2024..2099 is refused. The result goes to the board clock |
| `components/timezone` | `zones.h` (pure): 72 named zones -> POSIX rules, aliases (Europe/Kiev, GMT), a raw rule accepted; `timezone.h`: `TZ` + `tzset`, offset, DST |

Boot: `boards::Init()` sets the system clock from the board clock (if it has a
believable time), `config::Init`, then the zone; `timesync::Init` after the
Wi-Fi manager. A battery-backed RTC (X4 Pro BM8563) is a second `Clock`
later; until then the X4 Pro uses the timer clock too.

## UI

`components/ui`: LVGL 9.4 (`lvgl/lvgl`, as in approver-esp32, without
`esp_lvgl_port`), `ui::Init()` in `app_main` (`lv_init`, esp_timer tick).
`sdkconfig.defaults`: 1-bit colour (monochrome theme, for the X4 Pro e-ink),
custom allocator `lv_mem_psram.c`: every LVGL allocation (widgets, layers,
draw buffers) in PSRAM, internal RAM only if PSRAM is full, so Wi-Fi, BLE and
the web server keep the internal heap. No display, input device or
`lv_timer_handler` task yet: the X4 e-ink panel and GT911 touch drivers come
later. Version in `version`.

## BLE

NimBLE, peripheral, one connection. Stack is off at boot (`ble on`).

| Driver | Role |
|---|---|
| `ble` | stack on/off, advertising (HID + FIDO UUIDs), bonding (NVS), DIS + Battery services; profile drivers register GATT services and GAP listeners before `ble on` |
| `ble_kb` | HID over GATT keyboard; types ASCII with a US layout |
| `ble_fido` | FIDO BLE transport (service 0xFFFD, CTAP 2.1 BLE framing, MTU fragmentation); CTAP is a pluggable handler run in a worker task (MSG meanwhile: ERR_BUSY; CANCEL and KEEPALIVE frames), default answers "not supported" |

Security:
- IO capability DisplayOnly: the device shows a 6-digit passkey, the host types it (MITM-protected, LE Secure Connections).
- New hosts pair only inside the pairing window (`ble pair`); other pairing attempts are rejected. Opening it drops the current link, and bonded hosts reconnecting inside it are dropped, so the new host gets the only connection.
- One connection at a time. `ble use` picks the target bond: keys released on the old host, link dropped, advertising filtered (accept list) to the target; a non-target bonded host that still gets through is dropped after encryption. Without a target the accept list holds all bonds (no bonds: no advertising), so a host with a stale bond cannot reconnect in a loop; a bonded host that fails to encrypt is dropped.
- Unauthenticated (Just Works) links are dropped; HID reports and FIDO characteristics require an authenticated link.
- Windows 11 WebAuthn finds the FIDO service by Find By Type Value and fails (`_GetFidoGattService` 0x800710DF) unless its end handle matches Read By Group Type (NimBLE: 0xFFFF for the last service only): a tail service is registered after all profiles, see [windows-ble-fido.md](windows-ble-fido.md).
- GATT layout changes (hash in NVS `ble/gatt_hash`) are indicated to bonded hosts as Service Changed, so they re-read the table without re-pairing.

## Wi-Fi and web

Ported from approver-esp32 (`../../ai-remote/approver-esp32`, firmware.md
§10.9, web.md §10.16). Three layers:

| Layer | Role |
|---|---|
| `devices/wifi` (`wifi::Radio`) | join a network / be an AP / scan; no decisions. Stack (netifs + `esp_wifi_init`, ~40 KB) taken lazily, given back on stop. AP runs as APSTA so a scan works. Credentials never in the driver's NVS store |
| `wifimgr::Policy` (`wifi_policy.h`) | pure state machine, host-tested: which network, when to give up, when to be an AP |
| `wifimgr::Reachability` (`reachability.h`) | pure, host-tested: internet check rounds, online / offline / unknown |
| `wifimgr` manager | one task, 200 ms tick: radio status → policy events, policy actions → radio, SSIDs/passwords from config; ICMP echo (`esp_ping`) for the internet check |

Modes (`wifi.mode`, `wifi.active` false = off):
- `client`: networks in turn (last good first), gap between networks, growing
  backoff between rounds; after `rounds` passes, or at once when no network
  is left (none configured / every password refused), a temporary AP for
  `apWindowSeconds` (held open while a station is on it, endless with no
  networks), then the walk again. A wrong password is skipped until the
  window ends or the list changes; a drop while online is not a refusal.
- `ap`: the AP only.
- `apsta`: the AP always, plus the client walk; instead of the AP window it
  rests `apWindowSeconds` between cycles. While the station scans/joins, the
  AP follows its channel (one radio).

On a client link:
- Static address per network: set on association (DHCP client stopped first);
  a DHCP network after a static one starts the DHCP client again.
- Internet check: a round when the link comes up, then every
  `intervalSeconds`; targets in turn from the last one that answered, one
  reply = online; `failures` consecutive failed rounds = offline; unknown
  without a link or before the first answer. Never touches the link.
- Settings edits restart the walk (`wifimgr::Apply`, drops the link); the
  internet check settings do not (`ApplyInternetCheck`).

Web site (`components/web`): `esp_http_server` on port 80, pages embedded in
the firmware (`www/`), one stylesheet+script file fetched after the page.
`web.mode`: `off`, `on` (any network), `auto` (only while our AP is up).
Reconciled on the manager's tick, before its pump: the server closes its
socket before the radio destroys the netif (the other order is a null call),
and never starts without lwIP (an assert).

| Route | |
|---|---|
| `GET /`, `/*.html`, `/app.js` | pages: front (network, BLE, device), Wi-Fi settings, device status, restart. Whitelist of names/extensions; anything else, e.g. `/config.json`, is the 404 page |
| `GET /api/status` | firmware, heap, SPIFFS, Wi-Fi (IP, MAC, internet), AP, BLE, FIDO (readiness, requests, credentials stored of max, store bytes, PIN set and attempts, signature counter; `fido_store` false while a request holds the store). No secrets |
| `GET /api/devstatus` | the console's `devstatus` dump (stdout swapped for the request) |
| `GET /api/settings`, `POST /api/settings` | Wi-Fi mode, AP, networks (with `ip`; absent `ip` keeps the current one). Whitelist; passwords write-only (absent/null keeps, `""` clears); all-or-nothing; memory only |
| `GET /api/wifi/scan` | access points (hidden/duplicate names dropped) |
| `POST /api/action?do=save\|reload\|retry` | save config.json / reload it / put Wi-Fi settings on the radio |
| `POST /api/reboot?confirm=reboot` | restart |

- Basic auth over every route when `web.user` and `web.password` are both set
  (console only: `web login`); the header is compared to the encoded expected
  value, never decoded, constant-time. Not TLS.
- `web.write` false: settings and actions refused (403), pages read-only.
- Costs ~7 KB of internal heap while up.
- Names from the air (SSIDs) are inserted as text, never as HTML; JSON strings
  are escaped.
- Front page (polls `/api/status` every 5 s): Network (Wi-Fi state and mode,
  network, signal, address static/dhcp, MAC, internet, AP), Bluetooth (BLE,
  keyboard, FIDO2 readiness and counts; `stored` with a bar of the slots used,
  amber from 80 %, red from 90 %; PIN and attempts left; signature counter),
  Device (local time and zone, time sync, uptime, heap, PSRAM, SPIFFS with a bar of the space used, amber
  from 80 %, red from 95 %, firmware).
- Pages and `app.js` are cached for an hour: after a reflash, reload with
  Ctrl+F5 to get the new script.

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
- Reset only within 10 s after boot (lionkey; the window restarts after a
  successful reset). No U2F/CTAP1.
- Console (`fido2 info|list|delete|reset`): `fido_ctap_info/list/delete/reset`.
  A mutex serialises them with CTAP requests (a request holds it for its whole
  run, button wait included; the console waits 1 s, then "busy"). The console
  reset is authenticatorReset without the 10 s / button rule. A delete only
  flags the item: the file shrinks at the next boot (compaction).
- The store is plain text today (private keys, PIN hash): encryption and an SD
  store for hundreds of keys are planned, see [storage-encryption.md](storage-encryption.md).
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
