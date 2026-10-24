# Build, flash, CLI

## ESP-IDF

v6.0.2, installed by `eim` (manifest: `C:\Espressif\tools\eim_idf.json`). Stay on
6.0.x: the stock X4 Pro firmware is built with 6.0.1.
`idf.py` is a PowerShell function that exists only after sourcing the profile:

```powershell
pwsh -NoProfile -Command "& { . 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1' *> `$null; idf.py build }"
```

## Build and flash

Submodules: LionKey and only the parts of it the build uses (not tinyusb):

```powershell
git submodule update --init lib/lionkey
git -C lib/lionkey submodule update --init lib/tinycbor crypto/micro-ecc crypto/tiny-AES-c crypto/tinymt
```

```powershell
idf.py set-target esp32s3   # once
idf.py build
idf.py -p COM6 flash
```

- Dev board: bare ESP32-S3 (8 MB octal PSRAM, 16 MB flash) on USB Serial/JTAG,
  used until the X4 Pro is available.
- A board running a TinyUSB app (PID 4001) ignores DTR/RTS. Enter download
  mode by hand: hold BOOT, press RESET.
- Run `idf.py` from the project root or pass `-C <project>`: from a
  subdirectory (e.g. `host_test/`) it configures a build there.
- `flash` writes bootloader, partition table and app; SPIFFS (`config.json`,
  `fido.bin`) and NVS (bonds) are kept.
- If the app does not start after flashing, run
  `python -m esptool -p COM6 --before no-reset --after watchdog-reset chip-id`
  (esptool is in the IDF Python env: run it inside the profile).
- Console silent, panics, checks after a flash: [testing-on-device.md](testing-on-device.md).
- On the X4 Pro the app `.bin` is flashed through the CrossPoint unlocker-tool
  (see [architecture.md](architecture.md)).

## Version

`version.txt` + git → `PROJECT_VER`, e.g. `0.1.0-10-g060dff6[-dirty]`
(`cmake/version.cmake`). It is computed at configure time: run
`idf.py reconfigure` to pick up a new commit.

## Utils

`utils/` (Python 3, `pip install -r utils/requirements.txt`): console helpers (`console.py`: any command or the raw log; `spiffs.py`; `wallpaper.py`; `ble.py`), see [utils/README.md](utils/README.md).
Transfers are streamed in chunks, files are never loaded whole. `device.py` answers smart-mode cursor queries and switches the console to `term dumb` on connect.

```powershell
python utils/spiffs.py -p COM6 ls
python utils/spiffs.py -p COM6 info
python utils/spiffs.py -p COM6 rm <remote>
python utils/spiffs.py -p COM6 get <remote> [local]
python utils/spiffs.py -p COM6 put <local> [remote]
python utils/wallpaper.py -p COM6 put assets/*.png   # power-off wallpapers to the SD card
python utils/wallpaper.py -p COM6 ls | rm <name.png>
python utils/ble.py -p COM6 on | off | info | bonds | conns | disconnect
python utils/ble.py -p COM6 pair [seconds]     # waits, prints the passkey
python utils/ble.py -p COM6 pair [seconds] --bg | pair stop
python utils/ble.py -p COM6 use <#|addr|any>
python utils/ble.py -p COM6 unpair <#|addr|all>
python utils/ble.py -p COM6 kb "text"          # or kb --stdin; quoting and splitting
                                                # into console lines is done for you
python utils/screenshot.py -p COM7 [out.png]   # the e-ink screen as a PNG
```

## Tests

```powershell
host_test\run.cmd [suite...]   # host tests, no board (MSVC); see architecture.md
host_test\lionkey.cmd [regex]   # LionKey's GoogleTest suite in WSL (gcc, ASan/UBSan)
```

Suites: `config`, `wifi_policy`, `reachability`, `web_auth`, `web_paths`,
`web_settings`, `clock`, `boards`, `timesync`, `timezone`, `led`, `buttons`, `pin_policy`, `totp`. Run them after any change to those components.

`lionkey.cmd`: run after any change in `lib/lionkey`. Needs WSL Ubuntu with
gcc/g++ and CMake >= 3.30 + Ninja (on PATH or the release binaries in
`~/.local/opt`: `cmake-3.31.6-linux-x86_64/`, `ninja/`), and LionKey's
`lib/tinyusb` submodule (`git submodule update --init --recursive`). GoogleTest
and {fmt} are fetched on the first run; the build tree is
`~/.cache/esp32auth-lionkey-build` in WSL. The regex filters test names
(`CtapGetInfo`).

On the board (console, screen, `config test`, the FIDO2 suite `tests/fido2/`):
[testing-on-device.md](testing-on-device.md).

## Web site

Up when `web` says `running`; `web` prints the address(es).

- Default `web auto`: only while the device's own AP is up (`wifi mode ap` /
  `apsta`, or client mode's temporary AP). Join the AP (`esp32-auth`, default
  password `esp32-auth`: change it with `wifi ap`) and open http://192.168.4.1/.
- On a home network: `wifi set <ssid> <password>`, `web on`, then the IP from
  `web` / `wifi info`.
- Open site by default; `web login <user> <password>` locks every route.
- Quick API checks: [testing-on-device.md](testing-on-device.md#web-site).
- Front page: network (address, MAC, internet), BLE and the FIDO2 store
  (credentials used of 10, PIN, counter), device; the bars show what is used.
- Passwords page: unlock with the PIN, find, add, edit (password and TOTP
  write-only: an empty box keeps), delete (tap twice), the current TOTP code,
  lock. FIDO2 page: the credentials, delete (tap twice).
- Pages and `app.js` are cached for an hour: after a reflash reload with
  Ctrl+F5.

## CLI

`esp_console` REPL on USB Serial/JTAG (same port as the monitor), prompt `auth>`.
Opening the port with RTS asserted resets the chip.

| Command | Output |
|---|---|
| `help` | command list |
| `term` / `term smart` / `term dumb` | ask the terminal / force line editing + up-arrow history (32 commands) on / off. Off at boot: nobody answers the probe then. Smart mode needs a terminal that answers cursor queries (PuTTY does); if the console goes silent, reset the board |
| `version` | firmware version, build date, IDF version, ELF SHA256, chip, running slot |
| `info` | short: model, MACs, unique ID, flash size, CPU temperature, USB host (connected = host sends SOF, port open or not), reset reason, uptime |
| `reboot` | software reset (the USB console reconnects) |
| `hwinfo` | chip, MACs, eFuse unique ID, flash JEDEC/size, PSRAM, heap, eFuse security/download/USB bits, key blocks, NVS stats, flash layout (bootloader version, partitions, image sizes, gaps), OTA state (running/boot/next slot, rollback, raw otadata), CPU temperature, USB host, reset reason, uptime, clock (hardware RTC found or not, source, RTC timer, clock and system time to the ms, drift; no time: why, e.g. VL set), battery (as `battery`), e-ink (as `epd`), touch (as `touch`), SD card (as `sd`), LED (as `led info`) |
| `spiffs info` | partition address/size, mount point, total/used/free (esp_spiffs_info), file count |
| `spiffs ls` | SPIFFS files: size, modification time, file/fs totals |
| `spiffs cat <file>` | print a SPIFFS file as text |
| `spiffs catbase64 <file>` | print a SPIFFS file as base64, 76 chars per line |
| `spiffs write <file> [<length> <crc32>]` | prints `ready`, receives one base64 line (Enter ends the file), checks length/CRC32 (zlib), removes the file on error. Flow control: a `.` after every 192 base64 chars; send the next block only after it |
| `spiffs rm <file>` | delete a file |
| `spiffs format confirm` | erase the partition and create an empty fs (destroys stock data on the X4 Pro) |
| `ble on` / `ble off` | start / stop the BLE stack now (at boot: `ble boot`) |
| `ble info` | state, address, advertising, pairing window, connection (MTU, security), bonds, keyboard/FIDO readiness |
| `ble pair [seconds]` | open the pairing window (default 60 s) and wait: prints `passkey nnnnnn`, returns `paired with <addr>` or an error when the window closes |
| `ble pair <seconds> bg` / `ble pair stop` | open the window and return (passkey only in the log) / close it |
| `ble bonds` | numbered bond list (address, type), marks the connected and the target host |
| `ble conns` | the connection (one at a time): peer (identity + over-the-air address), security, key size, MTU, interval/latency/timeout, RSSI, Service Changed subscription, stored CCCDs (handle:bits, `*` pending) |
| `ble use <#>` / `<addr>` / `any` | switch hosts: all keys released on the current host, link dropped, advertising filtered to the target bond (kept in NVS) so only it reconnects; `any` lifts the filter. A pairing window advertises to all; a newly paired host becomes the target |
| `ble disconnect` | drop the link (keys released first); the host may reconnect |
| `ble unpair <#>` / `<addr>` / `all` | delete a bond by number from `ble bonds`, by address, or all; a connected host is disconnected |
| `ble boot [on\|off]` | show / set starting BLE at boot (`ble.active`, saved); `ble on`/`off` act now |
| `ble name [<name>]` | show / set the BLE name (1..29 bytes, words joined with spaces), applied at once and saved (`ble.name`) |
| `ble kb <text>` | type text on the connected host (US layout). esp_console drops single-backslash escapes: type `\\n` (Enter), `\\t` (Tab), `\\\\`; quote to keep spaces |
| `wifi on` / `wifi off` | radio on in the configured mode / off (saved) |
| `wifi mode client\|ap\|apsta` | set the mode and switch the radio on (saved) |
| `wifi info` | config, policy state (asked vs actual, timers), network and round, last error, radio mode, channel, MACs, hostname, IP (static/dhcp)/netmask/gateway/DNS, internet (state, last reply, failed rounds, next check), AP and its clients |
| `wifi scan` | access points: RSSI, channel, auth, SSID (brings the radio up for it if off) |
| `wifi ap [<ssid> [password] [channel]]` | show / set the device's own AP (empty password: open) |
| `wifi ping` | internet check round now, waits for it, then `wifi info` |
| `wifi check` / `wifi check on\|off` / `wifi check <ip> [ip ...]` | show the internet check / switch it / set up to 4 IPv4 targets (saved; the link is not touched) |
| `wifi retry` | restart the current mode from the top (re-reads the settings) |
| `wifi networks` | configured networks, in try order, static addresses, refused passwords marked |
| `wifi static <n> <ip> <mask> <gw> [dns1] [dns2]` / `wifi static <n> off` | fixed address for network n (all fields checked before saving) / back to DHCP, typed address kept |
| `wifi set <ssid> [password]` / `wifi forget <ssid\|all>` | add/update / remove a network (quote an SSID with spaces; no password = open) |
| `web [info]` | wanted mode, running, address(es), heap cost, auth, write, counters |
| `web on\|off\|auto` | serve on any network / never / only while the device's AP is up (saved) |
| `web login <user> <password>` / `web login off` | basic auth for the whole site / open site (saved). Both or neither; no ':' in the user |
| `web write on\|off` | allow / refuse settings changes from the site (saved) |
| `fido2 [info]` | credentials stored of max (resident), store bytes used of size, file size (encrypted), file version (and the highest seen here) and change time, PIN set, attempts left, minimum length (and how many sites may read it, "change due"), always uv, PIN complexity, built-in UV (not configured), the reset rule (window, button hold), signature counter; when FIDO2 is off, why (e.g. an old store file: rollback) |
| `fido2 list` | credentials: #, id (first 8 hex digits), resident, algorithm (ES256, EdDSA, Ed25519), site (RP ID, `...` if truncated), user name (display name), its origin: `made <local time> on <authenticator base MAC> by <host BLE identity address>` (`?` for what was not recorded, e.g. credentials made before) |
| `fido2 delete <#\|id>` | delete one: number from `fido2 list` (1-3 digits) or a unique id prefix (4+ hex digits; numbers shift after a delete) |
| `fido2 reset` / `fido2 reset confirm` | say what would go / delete every credential and the PIN (counter kept; the file becomes an empty store with the next version). Waits up to 1 s for a running FIDO request, else "busy". Also the way out of a refused store file |
| `sd [info]` / `sd ls [dir]` / `sd put <name.png> <length> <crc32>` / `sd rm <name.png>` | microSD: card (CID name), size, bus clock, mount point, FAT free of total, or why not mounted / a directory: type, size, local modification time, name (no `..`) / a wallpaper into `/sdcard/wallpapers` (one base64 line, as `spiffs write`; `utils/wallpaper.py`) / deletes one. Read-only but the wallpapers |
| `light [info]` / `light <1..100> [warmth]` / `light warmth <0..100>` / `light on\|off` | the frontlight: on/off, level, PWM duty per string / set the level (warmth 0 cool .. 100 warm, saved to `frontlight`) and light it / now only, not saved. Off at boot; Power x2 toggles. Fades 250 ms |
| `epd [info]` / `epd test [orient\|corners\|edges\|checker\|black\|white\|grey] [full\|fast]` / `epd sleep` | the e-ink panel: controller, both probe passes (FLG, VER), the stock screenType, refreshes and the last duration (full, fast or grey) / draw a test frame (orient: which way is up; corners, edges: what the bezel hides; grey: four bands black to white, UC8179 only) / deep sleep, the picture stays |
| `touch [info]` / `touch watch [s]` | the GT911: address, product, firmware, panel size, vendor, config head (version, output size, contacts, INT mode), frames and read errors, Home / each new frame that differs from the last for 1..120 s (repeats only counted: printing ~100 frames a second jammed the USB console until a replug): status, contacts raw and decoded (id, x, y, size) |
| `ui [info]` / `ui wallpaper [grey\|bw]` / `ui show\|key\|click\|swipe\|screenshot\|demo ...` | LVGL display: size, refreshes, the last one, the turn time, the ui task's free stack, touch (up/down, last point, presses) / draw a random power-off wallpaper now (as `display.wallpaper`, or as given), staying on / drive and read the screen: [testing-on-device.md](testing-on-device.md#screen) |
| `off [lock] [wake <s>]` | power-off: a wallpaper, deep sleep; Power wakes (a reboot). Also Power held, idle (`power.idleMinutes`); the vault key kept for `power.keepPinHours`, `lock` (also Power x3): not; `wake`: a timer wakes it too (tests) |
| `keys [info]` / `keys watch [s]` | the buttons: GPIO, raw and debounced level, held time, queue / events from now on for 1..120 s (default 10): pressed, released, long, click xN |
| `battery [info]` | gauge (CW2017 at 0x63, profile, last init action), voltage (mV), level (%), charging (STAT pin) |
| `battery regs` / `battery init` | gauge registers 0x00..0x0F and the resident profile, raw / check the profile again (loads it if it differs), reset the gauge |
| `board` | the board built for (`devices/config.h`): clock, display, touch, LED, battery, SD, frontlight, button and device pins |
| `led [info]` / `led <colour> [effect] [seconds]` | the board's WS2812: pin, UART, what it shows, ceilings, writes, task stack / show a colour (off white red green blue amber yellow cyan magenta) and effect (solid beacon slow blink fast breathe) for 1..600 s (default 5), then back |
| `led brightness [<n> [idle]]` | show / set the brightness ceilings in percent, normal and idle (saved) |
| `totp <base32 secret>` | local time (and UTC), seconds left in the window, the 6-digit codes of the previous, current and next 30 s window (HMAC-SHA1). Error without a believable time. The secret is not printed or logged, but stays in the up-arrow history |
| `totp test` | RFC 6238 SHA-1 vectors through `crypto::Totp` |
| `pass [info]` | password store: state (why it is off), records of max, file, version and change time |
| `pass list [filter]` | #, site / user, password set, TOTP; filter: part of site or user. Passwords are never shown |
| `pass add <site> [user] [password] [otp]` | a record; quote values with spaces. Prints its # (a new record goes last) |
| `pass set <#> <site\|user\|password\|otp> [value]` | change a field, no value: clear. Prints the record's new # |
| `pass delete <#>` / `pass code <#>` | delete a record / its TOTP codes now |
| `pass move <#> <to #>` / `pass sort` | the list order (the screen's): one record to a place / all by site, then user |
| `pass reset` / `pass reset confirm [pin]` | say what would go / delete every record. Locked or refused: the PIN is required and becomes the new store's PIN; the way out of a refused file |
| `nvs ls [namespace]` / `nvs rm <namespace> <key>` | development: NVS keys (namespace, key, type; never values) / erase one key |
| `pin [info]` | vault locked / open / refused, key derivation (iterations, ms) |
| `pin <6 digits>` | unlock the vault (nothing is decrypted before); no store yet: makes one with this PIN. `test.pin` in config.json does it at boot |
| `pin change <old> <new>` / `pin lock` | new PIN: a new key, `vault.bin` and `fido.bin` rewritten from memory together (`.new` + marker) / wipe the key, records and FIDO2 store from memory (FIDO2 off until the PIN) |
| `pin wipe set <pin>` / `pin wipe off` | the wipe PIN (needs the open store; must differ from the PIN), saved to config.json as a salted hash. Entering it with `pin` erases the password store and the FIDO2 credentials and answers "wrong PIN" |

`pin` also shows wrong PINs in a row and the lock: 10 wrong PINs lock PIN
entry for `pin.lockMinutes` (config.json), also across a reboot.
| `date [info]` | UTC and local time (offset, DST), zone, board clock (kept when off or not, drift from the system clock), sync (server, interval, next, last, step, failures) |
| `date sync` | ask the time server now and wait for it (up to 20 s) |
| `date set [utc] <YYYY-MM-DD> <HH:MM:SS>` | set the clock: local time, or UTC with `utc`; written to the board clock too |
| `date zone [<name\|rule>]` / `date zones [filter]` | show / set the time zone, a table name or a POSIX TZ rule (saved) / the zone table |
| `date server <host\|off>` / `date hours <0..168>` | SNTP server / sync interval, `off` or 0: no sync (saved) |
| `devstatus` | `version`, `info`, `board`, `date`, `battery`, `sd`, `led info`, `wifi info`, `web`, `ble info`, `fido2 info` in one dump (also the site's Device status page; its "Full hardware info" button shows `hwinfo`) |
| `config show` | current config as JSON, passwords masked (networks, AP, web) |
| `config info` | file path, source (file / built-in defaults), why the file was not used |
| `config test [pin <6 digits>\|off \| presence on\|off]` | development: `test.pin` and `test.autoPresence`, saved ([testing-on-device.md](testing-on-device.md#configjson-test)) |
| `config power [idle <0..1440 min>] [keep <0..168 h>] [rail on\|off]` | off when idle (0: never; never on USB) / a wake this soon after the sleep needs no PIN (0: always) / GPIO1 held through the sleep (off: on the battery the power drops), saved |
| `config wallpaper [grey\|bw]` | the power-off wallpaper: 4 grey levels (where the panel has them) or black and white (`display.wallpaper`), saved |
| `config reload` / `config save` | re-read config.json (keeps values on error) and apply Wi-Fi (incl. internet check), web, time and LED settings / write current values |
| `efuse` | every eFuse field (list generated at build time from IDF `esp_efuse_table.csv`) and raw blocks BLK0..BLK10 |
