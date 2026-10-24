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
| X4 Pro screen: status bar, credential list, pages, settings | status bar, the password list (pages, typing, TOTP), swipes, PIN pad, the U2F / FIDO2 list, Settings done |
| Settings: frontlight, Wi-Fi | on the screen: frontlight, Wi-Fi, Bluetooth, touch calibration, display & power, date & time, security, about; also the console and the site |
| Time: SNTP, zones, clock | done |
| Battery: voltage, level, charging | done (CW2017 + charger STAT, console `battery`, BLE Battery Level); screen not yet |
| Wi-Fi, configuration site, console | done |
| Power-off: a wallpaper on the screen, deep sleep, Power wakes | done (Power held, x3 locked, idle or `off`; PNGs in `/sdcard/wallpapers`; the PIN kept through a sleep: `power.keepPinHours`) |

## Stack

| Layer | Choice |
|---|---|
| Framework | ESP-IDF (C/C++) |
| UI | LVGL 9 (`components/ui`: 480x800 portrait on the e-ink panel, status bar, tabs, PIN pad; GT911 touch as the pointer; today: USB console + web site) |
| Host link | BLE HID keyboard (types credentials), BLE FIDO2 (CTAP2) |
| Network | Wi-Fi (client / AP / AP+client) and a configuration web site |
| Storage | `config.json` on SPIFFS; store files (`vault.bin`, `fido.bin`, encrypted) in `/sdcard/.authenticator` when the board has a card slot (no card: no stores, never a second set on SPIFFS), else SPIFFS |

Reference ESP-IDF + LVGL project: `../../ai-remote/approver-esp32` ([GitHub](https://github.com/merlokk/ai-remote/tree/main/approver-esp32)).

## Code layout

| Path | Contents |
|---|---|
| `main/` | `app_main`: OTA self-confirm, NVS, SPIFFS, config, console, Wi-Fi manager + web server, BLE autostart |
| `components/` | app components (`cli`: USB command line; `config`: config.json; `wifimgr`: Wi-Fi policy + manager; `web`: configuration site, see Wi-Fi and web; `fido_ctap`: CTAP 2.3 core, see FIDO2; `timesync`, `timezone`: see Time; `totp`: Base32 / Base64 secrets and the previous/current/next windows, codes from `crypto::Totp`; `ui`: LVGL and the power-off wallpaper, see UI; `power`: power-off, see UI; `storefile`: store file header, load/save with the rollback check; `vault`: passwords, see Passwords) |
| `lib/` | third-party git submodules (`lionkey`: our fork, see FIDO2) |
| `host_test/` | host tests (Unity, MSVC), no board: see Tests |
| `utils/` | host-side Python tools over the USB console (`device.py`: console session; `spiffs.py`: ls/info/rm/get/put; `wallpaper.py`: wallpapers on the card; `ble.py`: ble commands, `kb` text quoting) |
| `devices/` | hardware drivers, one component per device (`hardware`: SoC info, eFuse, USB host connected; `spiffs_fs`: SPIFFS on `spiffs` at `/spiffs`, never formatted, it holds stock data on the X4 Pro; `ble`, `ble_kb`, `ble_fido`: see BLE; `wifi`: the radio (`wifi::Radio`), no decisions, stores nothing; `buttons`: GPIO buttons, polled, debounced 25 ms (`button_logic.h`, pure: debounce, short/long press from approver-esp32; single/double/... press: `MultiPress`, 400 ms gap; `key_events.h`, pure: one `KeyTracker` per button: pressed / released edges, long at 600 ms (fired while held), click xN 400 ms after the last release, a press is long or a click, never both; a finger down at boot is no press), pins from the board; `keypad`: the one owner of the board's buttons, a task polling every 10 ms, events in a queue (16, full: dropped and counted) for the UI, listeners called from the task (bindings), `Pressed()` / `HeldMs()` for the rest (FIDO2 presence), console `keys`; `crypto`: hardware RNG, SHA-256, AES-256-CBC, AES-256-GCM, HMAC-SHA1 via PSA, HOTP/TOTP (`otp.h`, pure: counter, truncation); `clock`: `rtc::Clock`, see Time; `led`: one WS2812 from UART1, see Boards; `i2c_bus`: the board's I2C bus, leased (from approver-esp32), see Boards; `battery`: CW2017 fuel gauge and charger STAT, see Boards; `sd_card`: microSD over SDMMC, FAT at `/sdcard`, see Boards; `frontlight`: cool and warm LED strings on LEDC, see Boards; `touch`: the GT911 on the leased I2C bus (`gt911_logic.h`, pure), polled by the keypad task, see Boards; `epd`: the e-ink panel (SSD1677 / UC8179 / UC8279, detected), see Boards; `boards`: board classes, see Boards). `devices/config.h`: the board the firmware is built for |

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
- `power`: `idleMinutes` (0..1440, default 10, 0: never) off with nothing
  done; `keepPinHours` (0..168, default 0: the PIN after every wake) how
  long after a sleep a wake needs no PIN; `holdRail` (default true): GPIO1
  held high through the sleep (false: on the battery the power drops, no
  drain, the kept PIN lost).
- `test`: development aids (`pin`, `autoPresence`), see
  [testing-on-device.md](testing-on-device.md#configjson-test). Never on a
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
  "ble": {"active": false, "name": ""},
  "web": {"mode": "auto", "write": true, "user": "", "password": ""},
  "time": {"zone": "UTC", "server": "pool.ntp.org", "syncHours": 6},
  "led": {"brightness": 15, "idleBrightness": 7},
  "frontlight": {"brightness": 20, "warmth": 50},
  "display": {"wallpaper": "grey", "touch": {"xScale": 1000, "xOffset": 0, "yScale": 1000, "yOffset": 0}},
  "power": {"idleMinutes": 10, "keepPinHours": 0, "holdRail": true},
  "pin": {"lockMinutes": 10, "wipe": "", "complexity": true},
  "test": {"pin": "", "autoPresence": false}
}
```

`wifi.active` / `ble.active` start the radio at boot. `ble.name`: the
advertised and GAP name, 1..29 bytes of UTF-8 (the scan response); empty:
the board's (`ble_name`: `ESP32 Auth`, X4 Pro `ESP32 Auth X4Pro`), put into
the config at boot so a save keeps it. Applied at once (`ble name`, the
site); a paired host may show the old name until it reconnects. Up to 4 networks, tried
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
single/double press), `battery` (CW2017 VCELL, profile check, init decision),
`rtc` (BM8563 BCD, counters <-> UTC, VL, write plan), `pin_policy`, `totp` (truncation against the RFC 4226
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

`board_devices.h` carries it out: `boards::Init()` (LED, peripheral rail
high, I2C bus, the clock: BM8563 if found, else the timer), `I2c()`, `Clock()`, `Button(id)`, `PrintInfo()`
(`board`), `BootDone()`.

I2C (`devices/i2c_bus`, from approver-esp32): one `i2cbus::Bus` owned by the
board (`boards::I2c()`), 400 kHz; a task borrows it with `Acquire()` (a
`Lease`, released by scope; a sequence under one lease is atomic). A busy bus
is a skip after 50 ms, never a block; `Recover()` clocks a stuck SDA free.

Battery (`devices/battery`): CW2017 at 0x63 and the charger STAT line
(GPIO21, high = charging). `cw2017_logic.h` (pure): registers, the 80-byte
BATINFO profile from the stock firmware, VCELL (14 bits x 312.5 uV), the init
decision: none / soft reset / profile upload. `battery::Init()` after
`boards::Init()`: the gauge keeps the profile while the cell is connected
(stock loads the same one), so it is only written when it differs. Readouts:
`battery` (console), `[battery]` in `hwinfo`, the BLE Battery Level
(`battery::Level()`, 100 without a reading).

Touch (`devices/touch`, sequence from FreeInk): GT911, power GPIO2 active
low (50 ms), reset RST GPIO4 with INT GPIO10 low (address 0x5D; else high,
0x14), then it loads its own config (never written here). The keypad task
is its one owner: every 10 ms the status (0x814E), after a new frame (bit
0x80) the contacts (0x8150, 8 bytes each) and the clear. The capacitive
Home key (bit 0x10) is latched across frames (a still key makes none) and
is the keypad's fourth key (`ButtonId::kHome`, no GPIO): click / long
events like the others. Contacts are kept as the last frame for LVGL
(`touch::Contact`: the first contact; older than 100 ms, a lost lift frame,
counts as up). Console `touch` (info, `watch`), `[touch]` in `hwinfo`.

E-ink (`devices/epd`, from FreeInk, MIT): 800x480, 1 bit, MSB left, bit 1
white, 100 bytes a row. The controller is found at boot by reading FLG
(0x71) and VER (0x70) back over the data line (GPIO11 bit-banged as input,
two passes): an SSD1677 never answers, a UC81xx's VER byte 2 picks UC8179 or
UC8279. `epd_logic.h` (pure): detection, the stock screenType map, the RAM
row order per controller (UC8179: 600 gates, bottom row first, padding
after; UC8279: padding first; SSD1677: a Y-down window), BUSY polarity
(SSD1677 high = busy, UC81xx low = busy), the command bytes, the bezel
insets. The driver: SPI2 10 MHz mode 0, CS and DC by hand, planes through a
4 KB DMA buffer from a PSRAM frame; full and fast refresh (the old plane is
rewritten after each, so the next fast one diffs against the glass), the
panel powered off after each, deep sleep. Init draws nothing: the picture
on the glass stays. Console `epd` (info, test patterns, sleep).

Frontlight (`devices/frontlight`, from FreeInk): cool GPIO8 and warm GPIO9,
active high, LEDC 25 kHz / 10 bits (stock 7.x), LEDC channels 0 and 1.
`frontlight_logic.h` (pure): brightness 0..100 % to duty by a gamma-1.6554
table (1 % = 1 LSB, every percent distinct), warmth splits that total
between the strings (the sum stays the same; at warmth 50 each string gets
half, full power per string only at 0 or 100). Changes fade over 250 ms.
`config.json` `frontlight` {brightness 1..100 (default 20), warmth 50}: the
level `On()` lights; the on/off state is not kept, the light starts off.
Power x2 (`click x2`, a keypad listener in `main.cpp`) toggles it; console
`light <level>` (saved, lights it), `light on|off` (now only).

SD card (`devices/sd_card`): native SDMMC, 1-bit, FAT at `/sdcard`, mounted
once at boot (`sd_card::Init()` after `battery::Init()`). Sequence from
FreeInk: power off 80 ms / on 120 ms (GPIO5, active low) before each
attempt, mount, then a real read of sector 0; the power stays on.
`sd_policy.h` (pure): 40 MHz twice, then 20 MHz, at most 4 attempts; no
answer twice = no card; no FAT = left alone. Never formatted (the card holds
the reader's books). Long names in UTF-8 (`CONFIG_FATFS_API_ENCODING_UTF_8`).
No card-detect line. Console `sd` (read-only), `[sd card]` in `hwinfo`.
Measured: the stock 16 GB card mounts on the first attempt at 40 MHz, ~1 s
of boot.

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
| `devices/clock` | `rtc::Clock` interface: carries UTC to the next boot (`Read`/`Write`, `KeepsTimeWhenOff`). `TimerClock`: the chip's RTC timer + state in RTC no-init memory, kept by software resets, lost at power-off. `Bm8563Clock`: the X4 Pro's battery-backed RTC on the leased I2C bus; `Read` waits for the next tick (up to 1 s) for millisecond precision, `Write` stops the chip, writes the counters and starts it on a second boundary (first tick 1.000 s after STOP clears, measured). `clock_math.h` (pure): date <-> Unix, believable range 2024..2099, the timer state (magic + FNV check). `bm8563_logic.h` (pure): BCD, counters <-> UTC, VL (time lost), the write plan |
| `components/timesync` | SNTP (`esp_netif_sntp`), one task, 1 s tick. `sync_policy.h` (pure, from approver-esp32): sync at boot / when the internet appears / every `syncHours`; no re-sync within 5 min of a good one (flapping link); failure retry 1 min doubling to 15 min. Internet = a client link with an address not reported offline by the check. An answer outside 2024..2099 is refused. The result goes to the board clock |
| `components/timezone` | `zones.h` (pure): 72 named zones -> POSIX rules, aliases (Europe/Kiev, GMT), a raw rule accepted; `timezone.h`: `TZ` + `tzset`, offset, DST |

Boot: `boards::Init()` sets the system clock from the board clock (if it has a
believable time), `config::Init`, then the zone; `timesync::Init` after the
Wi-Fi manager. The X4 Pro uses the BM8563 (UTC, as the stock firmware keeps
it); not found: the timer clock. Measured: after `date set` the RTC and the
system clock agree within 1 ms; after a reboot the system clock is within
10 ms of the PC's.

## UI

`components/ui`: LVGL 9.4 (`lvgl/lvgl`, as in approver-esp32, without
`esp_lvgl_port`), `ui::Init()` in `app_main` after `epd::Init` (`lv_init`,
esp_timer tick; with a panel: the display and the `ui` task).
`sdkconfig.defaults`: 1-bit colour (monochrome theme), refresh period 200 ms
(e-ink is slow; FreeRTOS tick stays 100 Hz: the LVGL tick is esp_timer),
Montserrat 14 / 20 / 32, custom allocator `lv_mem_psram.c`: every LVGL
allocation (widgets, layers, draw buffers) in PSRAM, internal RAM only if
PSRAM is full, so Wi-Fi, BLE and the web server keep the internal heap.

Display: 480x800 portrait, I1, one screen buffer in PSRAM, DIRECT mode (LVGL
redraws dirty areas in place). On the last flushed area `ui_logic.h` (pure,
host-tested) turns the screen 90 degrees clockwise into the 800x480 panel
frame (portrait top = frame left, as held as a book; the bezel insets carry
over) into a pending frame; the `ui_push` task (4 KB) draws the latest one
with `epd::Refresh` (fast, every 20th full; a frame equal to the glass is
dropped), so the ui task keeps reading the touch through the second a
refresh takes (no tap lost). `ui::HoldPanel` keeps frames off the panel (a
wallpaper on it). `ui` task (8 KB stack, priority 3) runs `lv_timer_handler`; other tasks touch widgets only
inside `ui::Lock` / `ui::Unlock`. Measured on the X4 Pro (UC8179): turn 61 ms,
full refresh 1.9 s, stack used ~3.4 KB. The GT911 is LVGL's pointer, read
every 20 ms (the default 200 ms misses short taps); its coordinates are the
portrait screen's as they are (`TouchToScreen`, checked by eye: no swap or
flip).

Screens (`screens.cpp`; `status_logic.h`, `nav_logic.h`, `list_logic.h`
pure): the status bar on LVGL's top layer (over every screen and the PIN
pad): Wi-Fi (off: none; "..." trying, "!" no internet, "AP"), Bluetooth
("..." no host, "pair"), the microSD, the local time (once believable) and
the battery (icon by 25 %, charging mark, percent; moves by 2 % or more);
while FIDO2 waits, "FIDO2 <- yes -> no" instead of the left half. `main.cpp`
fills it (`ui::SetStatusSource`, every 0.5 s; the gauge every 30 s); a label
is set only when its text changed (each change is a fast refresh). The list:
the vault's records in the user's order, 6 a page; a row shows the site
(Montserrat 28), the TOTP code (32, redrawn each 30 s window), the user and
the password as `********` (24); one line over the rows says what a tap in
each quarter does; a tap in a quarter types the user, types the password, shows
the password (20 s, or a tap anywhere in the row, which then types nothing:
`list::OnTap`; wiped in LVGL's heap after) or types
the TOTP code (`ui::SetTypist` = `ble_kb::Type`, in the `ui_type` task; a
note says what happened). A tab per page at the bottom (none for one page),
at most 4: the row scrolls with the page, arrows where pages are hidden.
Left / Right pressed flip the pages (round), Home: page 1 (`ui::Navigate`,
queued from the keypad task; nothing under the PIN pad or while FIDO2
waits). The touch reports the finger's moves too: a swipe down from the top
quarter opens Settings (the frontlight, then pages; `settings_logic.h`, the
texts and actions from `main.cpp` through `ui::SettingsHooks`; the Wi-Fi
window: `wifi_view_logic.h`, `ui::WifiHooks`, the scan and the saves in the
ui's own task; Bluetooth: `ble_view_logic.h`, `ui::BleHooks`; Display &
power: `display_view_logic.h`, `ui::DisplayHooks`; Date & time:
`time_view_logic.h`, `ui::TimeHooks`; Security: `security_view_logic.h`,
`ui::SecurityHooks`, the PINs only in the ui's own task's job, wiped after; Web site:
`web_view_logic.h`, `ui::WebHooks`, the QR code by LVGL (`CONFIG_LV_USE_QRCODE`);
FIDO2 / U2F under Security (`fido_ctap_info`, Reset: `fido_ctap_reset`); the
FIDO2 prompt (`fido_prompt_logic.h` reads the request's CBOR / APDU before
the core, `main.cpp` adds the store's account, the window while the core
waits; Home allows as Left does); the
dialogs: `dialog.h` (`dialog_logic.h` the held Yes); the touch's
calibration: `touch_cal_logic.h`, applied to every real touch in `ui.cpp`), up from the bottom quarter the U2F / FIDO2
keys (`fido_ctap_list`, by site A to Z, 6 a page: site, user, made / alg /
kind; its pages flipped by Left / Right or the bar under it (‹, "1 / 2", ›, in the tabs' place); the count in its title; looked at
each 2 s, not while a request waits; a swipe left on a key: its trash can,
then "Delete this key?", No at once, Yes held 4 s, `fido_ctap_delete` in the
typing task: `fido_list_logic.h` `Remove`), both with a back
arrow and the name over them; the arrow (the list at
its page), the opposite swipe or Home closes them (`nav::Classify`; a slid tap
is no tap). FIDO2 presence: Left allows, Right refuses (OPERATION_DENIED,
`deny_pressed` hook).

PIN pad (`pin_pad.cpp`, `pin_pad_logic.h` pure): shown while the vault is
locked (polled each second with `vault::GetState`, no lock): at boot unless
`power.keepPinHours` kept the key, after `pin lock`; gone when the vault
opens, from the pad or the console. Six dots and a 3x4 pad (clear, 0,
backspace); the sixth digit is checked by `vault::Unlock` in a task of its
own (the derivation holds the vault for a second or more). No store yet: a
new PIN typed twice. Under the dots: tries left, the lock's minutes (redrawn
each minute), too simple. Keys have no pressed look (no refresh for it). The
wipe PIN works here as on the console. Only with the touch.
Console `ui`; version in `version`.

Power-off (`components/power`): the X4 Pro has no power switch, so "off" is
deep sleep. Power held (600 ms, the keypad's long press), Power x3 (locked),
idle or console `off [lock] [wake <s>]`: the ui task stops (its lock is never
given back), frontlight off, BLE off (the host sees the link closed), a random
PNG from `/sdcard/wallpapers` (the stock firmware's folder) on the panel
(full refresh), the panel in deep sleep, wait for Power to be released, then
RTC ext1 wake on Power (low, RTC pull-up) and deep sleep. Held through sleep
(`power_logic.h`, as FreeInk's PowerManager): EPD RST high, SD and touch
power off, the frontlight's PWM pins low, and (unlike FreeInk) GPIO1, the
peripheral rail, high (`power.holdRail`): on the battery the board's power drops without it (a
cold boot, RTC memory lost); `power::Boot()` drives them to the same levels
and releases them first thing at boot. A sleep
the chip refuses (`esp_deep_sleep_try_to_start` returns): a restart, not a
drain at full current. A wake is a reboot: the screens, the PIN again,
unless the key was kept. Kept key (`power.keepPinHours`, not after Power x3
or `off lock`): right before the sleep the vault key (`vault::CopyKey`) goes
into RTC memory (`RTC_NOINIT`: kept in deep sleep and resets, lost with the
battery) with the sleep time and a checksum (`Kept`, pure); `Boot()` takes
it out only after a deep-sleep wake and wipes RTC memory; then, with the
board clock in the system clock, `TakeKept` checks the time (from the
sleep, under the hours, a 2-minute slack back; the clock lost: no) and
`vault::Open(key)` opens the stores (another key in the file: locked,
nothing counted). Without secure boot and with the download mode open,
whoever holds the device could read it (as the 6-digit PIN can be brute-
forced offline from the files today). Idle (`power.idleMinutes`, read live):
a task checks every 5 s; activity is any key event, a touch press, BLE
typing, a FIDO2 request (start and end), a web request; a USB host (the
console, SOF frames: not a charger) or the pairing window keeps it on and
the idle time starts when it ends. Wallpaper (`wallpaper.cpp`,
`wallpaper_logic.h` pure): the PNG read whole into PSRAM (up to 4 MB),
LVGL's lodepng (`LV_USE_LODEPNG`; it returns an `lv_draw_buf_t`, RGBA),
grey (BT.601, alpha on white), centred (480x800 fills; larger is cropped),
Atkinson dither (cleaner whites than Floyd-Steinberg on engravings), turned
onto the panel. `display.wallpaper` (config.json, `config wallpaper`): `grey`
(default) dithers to 4 evenly spaced levels and draws them with
`epd::RefreshGrey` where the panel has grey (UC8179; else B/W), `bw` to 1 bit.
Grey (`epd_logic.h`, FreeInk's Direct mode as CrossPoint's sleep images): the
stock gray_full power registers and LUTs after a fresh init, the level's low
bit in the old plane, the high bit in the new one, one refresh (1.6 s on our
unit). The next B/W refresh re-inits, drives every pixel once (old = the frame
inverted, fast) and then runs full (2.6 s together): no grey shows through. No folder or no PNG: the screen stays as it was. Console
`ui wallpaper` draws one without sleeping; `sd put` / `sd rm`
(`utils/wallpaper.py`) write only plain `*.png` names into the folder: the
rest of the card stays read-only to the console.

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
| `GET /`, `/*.html`, `/app.js` | pages: front (network, device), settings (BLE state and name, wallpaper, frontlight, idle off, `holdRail`; each control set and saved at once), passwords, FIDO2, Wi-Fi settings, device status, hardware info (from device status), restart. Whitelist of names/extensions; anything else, e.g. `/config.json`, is the 404 page |
| `GET /api/status` | firmware, heap, SPIFFS, Wi-Fi (IP, MAC, internet), AP, BLE, FIDO (readiness, requests, credentials stored of max, store bytes, PIN set and attempts, signature counter, file version and modification time; `fido_store` false while a request holds the store, `fido_off` why FIDO2 is off), wallpaper, frontlight (present, on, level), `idle_minutes`, `hold_rail`, microSD (`sd`: mount outcome or `none`, CID name, card and FAT bytes, free), vault (`vault` state, records, wrong PINs, PIN lock left). No secrets |
| `GET /api/devstatus` | the console's `devstatus` dump (stdout swapped for the request) |
| `GET /api/hwinfo` | the console's `hwinfo` dump, the same way (eFuse key purposes and flags only, no NVS values) |
| `GET /api/settings`, `POST /api/settings` | Wi-Fi mode, AP, networks (with `ip`; absent `ip` keeps the current one), `ble.name` (applied to the radio at once), `display.wallpaper` (`grey` / `bw`), `frontlight` {`brightness` 1..100, `warmth` 0..100} (applied to the light at once), `power` {`idleMinutes`, `holdRail`; not `keepPinHours`}; the settings page saves each. Whitelist; passwords write-only (absent/null keeps, `""` clears); all-or-nothing; memory only |
| `GET /api/wifi/scan` | access points (hidden/duplicate names dropped) |
| `POST /api/action?do=save\|reload\|retry\|light-on\|light-off` | save config.json / reload it / put Wi-Fi settings on the radio / the frontlight on or off (not saved) |
| `POST /api/reboot?confirm=reboot` | restart |
| `GET /api/vault` | store state, PIN tries / lock, records: id, site, user, whether a password / TOTP secret is set, modified. Never a password or secret |
| `GET /api/vault/totp?id=N` | the record's code for the current window and the seconds left |
| `POST /api/vault/add\|set\|delete` | `{site, user, password, otp}` / `{id, any of them}` (absent keeps, `otp: ""` removes; all checked before any is written) / `{id}`. `otp`: Base32, or `base64:<Base64>` (kept as Base32) |
| `POST /api/vault/move\|sort` | `{id, position}` (0-based list place; past the end: last) / `{}` (by site, then user): the device's list order; the page shows it with up / down per record and "Sort A to Z" |
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
  keyboard, FIDO2 readiness and counts), Security (PIN: locked / unlocked,
  wrong PINs, lockout; FIDO2 keys with a bar of the slots used, amber from
  80 %, red from 90 %; FIDO2 PIN and attempts left; signature counter; file
  version and change time; why FIDO2 is off, if it is), Device (local time and zone,
  time sync, uptime, heap, PSRAM, SPIFFS with a bar of the space used, amber
  from 80 %, red from 95 %; the microSD, connected and free space, a bar amber
  from 90 %, red from 98 %, hidden without a slot; firmware).
- Settings page: BLE name, power-off wallpaper, frontlight, `idleMinutes`,
  `holdRail`, each set and saved at once.
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
  `fido_max_credentials` (dev board 20 on SPIFFS, X4 Pro 125 on the SD card;
  1..128: LionKey lists at most 128 per site and 128 sites); lionkey stores
  non-discoverable ones too, one more gets KEY_STORE_FULL. The RAM log
  (`FIDO_CTAP_STORE_BYTES`: 1 KB per credential + 2 KB, 22 KB for 20) and the
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
  board; debounced by the keypad task) within 30 s; KEEPALIVE every 300 ms; CANCEL aborts the wait.
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
`main.cpp`: `/sdcard/.authenticator` when the board has a card slot, made
at boot; no card or not mounted: no stores until a reboot with one; else
SPIFFS).

- Records (`records.h`, pure, host-tested): site (1-96 bytes), user (0-96),
  password (0-128), TOTP secret (Base32, normalized, up to 64 characters;
  `base64:<Base64>` is taken and kept as Base32);
  UTF-8 without control characters. A site may have several users; (site,
  user) is unique without ASCII case. In the user's order (the screen's
  and the site's): a new record goes last, `Move` puts one elsewhere,
  `SortByName` sorts by site, then user; serialized in that order (format
  2; format 1 files, kept sorted by name, are read sorted). Each record has
  a stable ID (never reused) and its own modification time.
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
