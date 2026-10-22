# Architecture

## Functionality

Target and status. Screen design and the open questions: [x4-design.md](x4-design.md).

| Function | Status |
|---|---|
| FIDO2 key (CTAP 2.3, U2F) over BLE | done |
| FIDO2 over USB | not yet |
| BLE keyboard: types login / password / TOTP | keyboard done; typing records not yet |
| TOTP codes | done (`crypto::Totp`, console `totp`) |
| PIN lock (6 digits, lockout after 10 tries, wipe PIN); the PIN also derives the storage key | PIN key for `vault.bin`, lockout, wipe PIN, console `pin` done; screen not yet |
| Password records (site, user, password, TOTP): add / edit / delete | done on the console (`pass`); screen and typing not yet |
| Store file header (version, modification time) and rollback check (highest version in NVS) | done (`storefile`) |
| Encrypted stores: `vault.bin` and `fido.bin`, AES-256-GCM with a key from the PIN (the same on every device with the PIN and the files), loaded whole into PSRAM | done |
| Sync over the internet, the later modification time wins | not yet; service not chosen |
| X4 Pro screen: status bar, credential list, pages, settings | not yet (LVGL up, no display driver) |
| Settings: frontlight, Wi-Fi | Wi-Fi on the console and the site; frontlight not yet |
| Time: SNTP, zones, clock | done |
| Wi-Fi, configuration site, console | done |

## Stack

| Layer | Choice |
|---|---|
| Framework | ESP-IDF (C/C++) |
| UI | LVGL 9 (`components/ui`: library up, no display driver yet; today: USB console + web site) |
| Host link | BLE HID keyboard (types credentials), BLE FIDO2 (CTAP2) |
| Network | Wi-Fi (client / AP / AP+client) and a configuration web site |
| Storage | `config.json` on SPIFFS; store files (`vault.bin`, `fido.bin`, encrypted) on the SD card when the board has one, else SPIFFS |

Reference ESP-IDF + LVGL project: `../../ai-remote/approver-esp32` ([GitHub](https://github.com/merlokk/ai-remote/tree/main/approver-esp32)).

## Code layout

| Path | Contents |
|---|---|
| `main/` | `app_main`: OTA self-confirm, NVS, SPIFFS, config, console, Wi-Fi manager + web server, BLE autostart |
| `components/` | app components (`cli`: USB command line; `config`: config.json; `wifimgr`: Wi-Fi policy + manager; `web`: configuration site, see Wi-Fi and web; `fido_ctap`: CTAP 2.3 core, see FIDO2; `timesync`, `timezone`: see Time; `totp`: Base32 / Base64 secrets and the previous/current/next windows, codes from `crypto::Totp`; `ui`: LVGL, see UI; `storefile`: store file header, load/save with the rollback check; `vault`: passwords, see Passwords) |
| `lib/` | third-party git submodules (`lionkey`: our fork, see FIDO2) |
| `host_test/` | host tests (Unity, MSVC), no board: see Tests |
| `utils/` | host-side Python tools over the USB console (`device.py`: console session; `spiffs.py`: ls/info/rm/get/put; `ble.py`: ble commands, `kb` text quoting) |
| `devices/` | hardware drivers, one component per device (`hardware`: SoC info, eFuse, USB host connected; `spiffs_fs`: SPIFFS on `spiffs` at `/spiffs`, never formatted, it holds stock data on the X4 Pro; `ble`, `ble_kb`, `ble_fido`: see BLE; `wifi`: the radio (`wifi::Radio`), no decisions, stores nothing; `buttons`: GPIO buttons, polled, debounced 25 ms (`button_logic.h`, pure: debounce, short/long press from approver-esp32; single/double/... press: `MultiPress`, 400 ms gap), pins from the board; `crypto`: hardware RNG, SHA-256, AES-256-CBC, AES-256-GCM, HMAC-SHA1 via PSA, HOTP/TOTP (`otp.h`, pure: counter, truncation); `clock`: `rtc::Clock`, see Time; `led`: one WS2812 from UART1, see Boards; `boards`: board classes, see Boards). `devices/config.h`: the board the firmware is built for |

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
- `pin`: `lockMinutes` (1..1440, default 10) after 10 wrong PINs; `wipe`:
  the wipe PIN as `<salt hex>:<SHA-256(salt || PIN) hex>` (set by
  `pin wipe set`, masked by `config show`), never the PIN itself.
  `complexity` (default true): a new PIN, vault and FIDO2, must pass
  `pin_policy.h` (no repeats, runs or common PINs).
- `test`: development aids. `test.pin` (empty or 6 digits) unlocks the vault
  at boot; masked by `config show`, never on the site. `test.autoPresence`
  (default false): FIDO2 / U2F user presence is confirmed without the button
  (a log warning each time; `fido2 info` says so; not the reset's 5 s hold);
  read at every request, so `config set` takes effect at once. Never on a
  device with real secrets.
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
  "led": {"brightness": 15, "idleBrightness": 7},
  "pin": {"lockMinutes": 10, "wipe": "", "complexity": true},
  "test": {"pin": ""}
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
`boards`, `timesync`, `timezone`, `led`, `buttons` (debounce, short/long,
single/double press), `pin_policy`, `totp` (truncation against the RFC 4226
HMAC values; the HMAC itself is checked on the board by `totp test`); only code with
no hardware (pure headers) is under test.

```powershell
host_test\run.cmd            # all suites
host_test\run.cmd config     # suites whose name contains "config"
```

`host_test/lionkey.sh` (`lionkey.cmd` from Windows): LionKey's own GoogleTest
suite (`lib/lionkey/test`), built by gcc in WSL with ASan/UBSan (MSVC cannot
build it: C VLAs).

## Boards

`devices/boards`: one class per board, deriving from `boards::Board`
(`board_spec.h`, pure): `hw_clock` (none, BM8563), `display` (none, e-ink
800x480), `touch` (none, GT911), `led` (none, WS2812), `battery` (none,
CW2017), `has_sd_card`, `has_frontlight`, and GPIOs for the buttons (Left,
Right, Power; Left is FIDO2 user presence) and every device, and
`fido_max_credentials` (the size of `fido.bin`). Chosen at build
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
breathing at the idle ceiling when `app_main` is done. Overrides: blue solid
while the BLE keyboard types (until 1 s after the last key), yellow fast
blinking while FIDO2 reset waits for the 5 s hold (solid while held), yellow blinking
(500/500 ms) while FIDO2 waits for the button.

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
refresh period 200 ms (e-ink is slow; FreeRTOS tick stays
100 Hz: the LVGL tick is esp_timer), custom allocator `lv_mem_psram.c`: every LVGL allocation (widgets, layers,
draw buffers) in PSRAM, internal RAM only if PSRAM is full, so Wi-Fi, BLE and
the web server keep the internal heap. No display, input device or
`lv_timer_handler` task yet: the X4 e-ink panel and GT911 touch drivers come
later. Version in `version`.

## BLE

NimBLE, peripheral, one connection. Stack is off at boot (`ble on`).

| Driver | Role |
|---|---|
| `ble` | stack on/off, advertising (HID + FIDO UUIDs), bonding (NVS), DIS (serial number = base MAC) + Battery services; profile drivers register GATT services and GAP listeners before `ble on` |
| `ble_kb` | HID over GATT keyboard; types ASCII with a US layout |
| `ble_fido` | FIDO BLE transport (service 0xFFFD, CTAP 2.1 BLE framing, MTU fragmentation; at MTU 23 the worker waits for free NimBLE mbufs instead of dropping frames); CTAP is a pluggable handler run in a worker task (MSG meanwhile: ERR_BUSY; CANCEL and KEEPALIVE frames), default answers "not supported" |

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
| `GET /`, `/*.html`, `/app.js` | pages: front (network, BLE, device), passwords, FIDO2, Wi-Fi settings, device status, hardware info (from device status), restart. Whitelist of names/extensions; anything else, e.g. `/config.json`, is the 404 page |
| `GET /api/status` | firmware, heap, SPIFFS, Wi-Fi (IP, MAC, internet), AP, BLE, FIDO (readiness, requests, credentials stored of max, store bytes, PIN set and attempts, signature counter, file version and modification time; `fido_store` false while a request holds the store, `fido_off` why FIDO2 is off). No secrets |
| `GET /api/devstatus` | the console's `devstatus` dump (stdout swapped for the request) |
| `GET /api/hwinfo` | the console's `hwinfo` dump, the same way (eFuse key purposes and flags only, no NVS values) |
| `GET /api/settings`, `POST /api/settings` | Wi-Fi mode, AP, networks (with `ip`; absent `ip` keeps the current one). Whitelist; passwords write-only (absent/null keeps, `""` clears); all-or-nothing; memory only |
| `GET /api/wifi/scan` | access points (hidden/duplicate names dropped) |
| `POST /api/action?do=save\|reload\|retry` | save config.json / reload it / put Wi-Fi settings on the radio |
| `POST /api/reboot?confirm=reboot` | restart |
| `GET /api/vault` | store state, PIN tries / lock, records: id, site, user, whether a password / TOTP secret is set, modified. Never a password or secret |
| `GET /api/vault/totp?id=N` | the record's code for the current window and the seconds left |
| `POST /api/vault/add\|set\|delete` | `{site, user, password, otp}` / `{id, any of them}` (absent keeps, `otp: ""` removes; all checked before any is written) / `{id}`. `otp`: Base32, or `base64:<Base64>` (kept as Base32) |
| `POST /api/vault/unlock\|lock` | `{pin}` (the wipe PIN answers as a wrong one) / `{}` |
| `GET /api/fido`, `POST /api/fido/delete` | store info and credentials (site, user, resident, U2F, made when / by; the page marks U2F ones with a badge) / `{credential: 64 hex}` |

- Basic auth over every route when `web.user` and `web.password` are both set
  (console only: `web login`); the header is compared to the encoded expected
  value, never decoded, constant-time. Not TLS.
- `web.write` false: settings and actions refused (403), pages read-only.
- Vault / FIDO2 POSTs (`web_admin.h`, pure): a field whitelist, only
  `Content-Type: application/json` (a cross-site form cannot send it without
  a preflight), the body and parsed strings wiped after use; logs carry the
  verb and outcome only.
- Costs ~7 KB of internal heap while up.
- Names from the air (SSIDs) are inserted as text, never as HTML; JSON strings
  are escaped.
- Front page (polls `/api/status` every 5 s; a click on the lamp, top right,
  reads it at once): Network (Wi-Fi state and mode,
  network, signal, address static/dhcp, MAC, internet, AP), Bluetooth (BLE,
  keyboard, FIDO2 readiness and counts; `stored` with a bar of the slots used,
  amber from 80 %, red from 90 %; PIN and attempts left; signature counter;
  file version and change time; why FIDO2 is off, if it is),
  Device (local time and zone, time sync, uptime, heap, PSRAM, SPIFFS with a bar of the space used, amber
  from 80 %, red from 95 %, firmware).
- Pages and `app.js` are cached for an hour: after a reflash, reload with
  Ctrl+F5 to get the new script.

## FIDO2

`components/fido_ctap`: LionKey CTAP 2.3 core (`lib/lionkey`, git submodule:
our fork [merlokk/lionkey](https://github.com/merlokk/lionkey), branch
`esp32auth`, of [pokusew/lionkey](https://github.com/pokusew/lionkey)) behind
`ble_fido`. API: `fido_ctap_init(hooks, store)`,
`fido_ctap_request(msg) -> response`.

- Crypto: lionkey's table with RNG, SHA-256, AES from the `crypto` driver;
  P-256 on lionkey's micro-ecc (no ECC accelerator on the S3; its `uECC_*`
  symbols are renamed `lk_uECC_*`, NimBLE's tinycrypt has the same names).
- Store: `/spiffs/fido.bin` = store header + lionkey's RAM log, AES-256-GCM
  with the vault's key (Passwords), rewritten whole through a temp file on
  every change, compacted at load. FIDO2 is off from boot until the PIN
  (`fido_ctap_set_key` through `vault::Hooks`) and again after `pin lock`;
  a plain file, or one with another key id, is refused. A PIN change
  re-keys it (`fido_ctap_stage_key` / `commit_key`).
- Store header (`components/storefile/store_header.h`, pure, host-tested):
  64 bytes, plain, in front of every store file: magic `ESAF`, format, kind,
  cipher (0 plain, 1 AES-256-GCM: the header is its associated data),
  **version**, payload length, **modification time** (UTC, 0 unknown), nonce,
  PBKDF2 iterations and salt, **key id**. Every write: version = max(file,
  NVS) + 1; NVS `fido/store_ver` keeps the highest version accepted (written
  after the file, only when it grows). At boot a file below it is a
  rollback: refused and left as is, FIDO2 off (`fido_ctap_stopped_reason`,
  console and site); `fido2 reset confirm` writes an empty store over it. A
  file above NVS (power loss after the file, or a synced copy) is accepted.
  No file at all with a version kept (deleted, SPIFFS formatted) is refused
  too (an old copy would otherwise follow an empty one); `fido2 reset`
  starts an empty store above the kept version. A save is .tmp, remove, rename: at boot a .tmp with no
  file is taken, a .tmp next to the file dropped. `Newer()` holds the sync rule. Max credentials: the board's
  `fido_max_credentials` (15 on the dev board, 10 on the X4 Pro, 1..50); lionkey stores
  non-discoverable ones too, one more gets KEY_STORE_FULL. The RAM log
  (`FIDO_CTAP_STORE_BYTES`: 1 KB per credential + 2 KB, 12 KB for 10) and the
  compaction buffer are in PSRAM. Next to each credential the store keeps a
  origin item of its own (key 0x100, ignored by lionkey; fixed little-endian
  layout in `ctap_file_storage.h`, it travels with the store when devices
  sync): credential ID, transport and the host's BLE identity address, the
  creation time (UTC, 0 without a believable time), this authenticator's base
  MAC (which device made it). Made, replaced and deleted with the credential;
  shown by `fido2 list`. A file larger than the log (limit lowered)
  is not loaded or touched: FIDO2 stays off.
  The signature counter is in NVS (`fido/sign_count`), kept by reset.
- User presence: a fresh press of the Left button (GPIO0, BOOT on the dev
  board, debounced) within 30 s; KEEPALIVE every 300 ms; CANCEL aborts the wait.
- Reset (authenticatorReset): only within 10 s after the core starts (boot or
  PIN unlock; lionkey; the window restarts after a successful reset), then a
  5 s hold of Left (CTAP 2.3 longTouchForReset, getInfo 0x18 = true; a hold
  released early is not a refusal, the next press starts over; 30 s, CANCEL as
  above). The window is the board's `fido_reset_window_s` (0 = any time).
- U2F (CTAP1) over BLE: register / authenticate / version (LionKey fork
  `ctap1.c`); a self-signed per-device attestation certificate; the BLE
  service offers U2F 1.2 and FIDO2.
- authenticatorConfig (LionKey fork): toggleAlwaysUv, setMinPINLength
  (only raises; a PIN below it must be changed: forcePINChange),
  enableLongTouchForReset; kept in `fido.bin` (storage key 4), cleared by a
  reset. minPinLength extension: the minimum PIN length goes to up to 8 sites
  set by setMinPINLength (storage key 5, SHA-256 of the RP IDs). PIN complexity
  policy (`pin.complexity`, or setMinPINLength until a reset): getInfo 0x1B,
  the `pinComplexityPolicy` extension. Algorithms: ES256 (micro-ecc) and
  EdDSA / Ed25519 (COSE -8, -19; Monocypher), the host's first choice. hmac-secret
  also at makeCredential (`hmac-secret-mc`, PRF at create). Read-only credential
  management with a persistent pinUvAuthToken (`pcmr`, storage key 7,
  dropped by a PIN change or a reset). U2F attestation key and certificate:
  storage key 6; the encIdentifier device id: key 8. Attestation: packed (self) or `none` on
  request (`attestationFormatsPreference`). getInfo encIdentifier / encCredStoreState
  (encrypted with the persistent token; a random device id, a hash of the store).
  Built-in UV: `uv: false` (supported, not configured; the PIN on the device
  later), requests only logged. getInfo versions: U2F_V2, FIDO_2_0, FIDO_2_1,
  FIDO_2_3. Details: [lionkey-update-plan.md](lionkey-update-plan.md).
- Console (`fido2 info|list|delete|reset`): `fido_ctap_info/list/delete/reset`.
  A mutex serialises them with CTAP requests (a request holds it for its whole
  run, button wait included; the console waits 1 s, then "busy"). The console
  reset is authenticatorReset without the 10 s / button rule. A delete only
  flags the item: the file shrinks at the next boot (compaction).
- An SD store for hundreds of keys: [storage-encryption.md](storage-encryption.md).
- Tests: `tests/fido2` over BLE, all 67 pass on the dev board (see build.md).

## Passwords

`components/vault`: `vault.bin` in the store directory (`StorageDir()` in
`main.cpp`: the SD card when the board has one, else SPIFFS; no SD driver yet,
so no store files on the X4 Pro).

- Records (`records.h`, pure, host-tested): site (1-96 bytes), user (0-96),
  password (0-128), TOTP secret (Base32, normalized, up to 64 characters;
  `base64:<Base64>` is taken and kept as Base32);
  UTF-8 without control characters. A site may have several users; (site,
  user) is unique without ASCII case. Sorted by site, then user; each record
  has a stable ID (never reused) and its own modification time.
- File: store header (kind 2, AES-256-GCM, random nonce per write; the header
  is the associated data) + the serialized records + tag. Loaded whole and
  decrypted into PSRAM at unlock, rewritten whole on every change (version,
  `.tmp`, rollback check as for `fido.bin`, through `storefile/store_io`).
  A write that fails reloads the file: the change is undone.
- Locked from boot until the PIN (console `pin <6 digits>`, or `test.pin`
  in config.json): nothing is decrypted before. No file yet: the first PIN
  makes an empty store and the key. A wrong PIN (the derived key id differs
  from the header's) leaves it locked; on an open store it changes nothing.
  `pin lock` wipes the key, records and the FIDO2 store from memory.
- Key: PBKDF2-HMAC-SHA256(PIN, salt), 6000 iterations (~0.9 s on the S3 via
  PSA); salt, count and the key id (SHA-256("ESAF key id" || key), 8 bytes)
  in every store header. Another device with the PIN and the files (a sync
  server) derives the same key; checked by building the files on a PC.
  Against a flash dump a 6-digit PIN falls in minutes: see
  storage-encryption.md.
- PIN change (needs the store open, and fido.bin loaded if it exists): a new
  key (new salt); both stores are written from memory to `<file>.new`, then
  the marker `pinchange`, then each `.new` renamed over its file, then the
  marker removed (`storefile::Stage` / `BeginCommit` / `Promote` /
  `EndCommit`). At boot, before any load, `storefile::FinishCommit`: with
  the marker the `.new` files are complete and renamed; without it they are
  dropped and the old PIN stands.
- Lockout (`pin_guard.h`, pure, host-tested): 10 wrong PINs in a row
  (unlock, or the old PIN of a change) lock PIN entry for `pin.lockMinutes`,
  then 10 more tries. Count and deadline (UTC) in NVS (`vault/pin_fail`,
  `vault/pin_until`): a reboot resumes the lock with a known clock, else
  (or with a deadline further than a full lock) starts it again. While
  running it counts on the uptime clock.
- Wipe PIN (`pin.wipe`): checked first, before any key derivation and also
  while locked out. It removes `vault.bin`, `fido.bin` (and any `.new`,
  `.tmp`, marker) and their kept versions (the next PIN starts afresh),
  wipes the key and records from memory and stops FIDO2 (`fido_ctap_wipe`
  through `vault::Hooks`). Then it
  answers as a wrong PIN: same result and count (it can start the lock),
  one key derivation, the same log line; the erase adds ~0.4 s. It must
  differ from the PIN (checked when set, on a PIN change and a reset).
- Refused (rollback, missing while a version is kept, malformed, does not
  decrypt): left as is, the store is off; `pass reset confirm` (with the key
  in) or `pass reset confirm <pin>` (a new key from that PIN: `fido.bin`
  then needs `fido2 reset`) writes an empty store over it.
- PSRAM: ~1.2 KB per record of the board's `max_records` (dev board 50, X4
  Pro 500): the records, the plaintext and the file body buffers.
- Console `pass`; passwords are never printed (list shows "set"), only typed
  in (they stay in the up-arrow history).

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
