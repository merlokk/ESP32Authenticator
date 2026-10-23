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
- Console silent (commands time out, writes to COM6 block): the app is hung or
  boot-looping. Read the port without writing (pyserial, DTR/RTS low) to get
  the boot log and panic backtrace, then decode it:
  `xtensa-esp32s3-elf-addr2line -pfiaC -e build/esp32-authenticator.elf <addr...>`.
- On the X4 Pro the app `.bin` is flashed through the CrossPoint unlocker-tool
  (see [architecture.md](architecture.md)).

## Version

`version.txt` + git → `PROJECT_VER`, e.g. `0.1.0-10-g060dff6[-dirty]`
(`cmake/version.cmake`). It is computed at configure time: run
`idf.py reconfigure` to pick up a new commit.

## Utils

`utils/` (Python 3, `pip install -r utils/requirements.txt`): console helpers (`console.py`: any command or the raw log; `spiffs.py`; `ble.py`), see [utils/README.md](utils/README.md).
Transfers are streamed in chunks, files are never loaded whole. `device.py` answers smart-mode cursor queries and switches the console to `term dumb` on connect.

```powershell
python utils/spiffs.py -p COM6 ls
python utils/spiffs.py -p COM6 info
python utils/spiffs.py -p COM6 rm <remote>
python utils/spiffs.py -p COM6 get <remote> [local]
python utils/spiffs.py -p COM6 put <local> [remote]
python utils/ble.py -p COM6 on | off | info | bonds | conns | disconnect
python utils/ble.py -p COM6 pair [seconds]     # waits, prints the passkey
python utils/ble.py -p COM6 pair [seconds] --bg | pair stop
python utils/ble.py -p COM6 use <#|addr|any>
python utils/ble.py -p COM6 unpair <#|addr|all>
python utils/ble.py -p COM6 kb "text"          # or kb --stdin; quoting and splitting
                                                # into console lines is done for you
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

`tests/fido2/`: CTAP2 authenticator suite (pytest + python-fido2,
`pip install -r tests/fido2/requirements.txt`). Transport-agnostic, checked
against a reference USB key (YubiKey). On Windows both transports need an
administrator terminal (FIDO HID and the GATT FIDO service are hidden otherwise).

- `--transport hid` (default): USB CTAPHID.
- `--transport ble --device <address|name>`: FIDO over BLE (`ble_transport.py`,
  bleak). Pair the device with the PC first (`ble pair`, passkey in Windows
  Bluetooth settings). A bonded device already connected to the PC does not
  advertise: pass its address, the scan is skipped.
- Power cycle (replug, reset tests): on USB the user replugs the key; over BLE
  `--console COM6` sends `reboot` to our firmware, waits for `ble info` to say
  `state on` (the console drops commands right after boot), reconnects.
  Without `--console` these tests are skipped on BLE.
- `test_transport.py` (marker `transport`): PING on both transports, BLE
  framing errors. If getInfo fails (no CTAP2 core yet), only these run.

```powershell
cd tests/fido2
python -m pytest [-k name] [--transport ble] [--device YubiKey|<address>] [--console COM6] [--pin 1234] [--set-pin] [--destructive] [--reset]
```

- Never changes the PIN; sets one only with `--set-pin`, resets only with `--reset`.
- Non-resident credentials of the test RPs (`ctap-test*.example`) can stay on
  our firmware after a run (U2F ones show as `U2F 3dec69ac`): check with `fido2 list`, remove with
  `fido2 delete <id>` (the store holds 20; a run leaves ~6, a full store fails with
  KEY_STORE_FULL / U2F 0x6986).
- PIN: asked at start when the key has one (or `--pin` / `FIDO_PIN`); empty
  skips the PIN, UV and discoverable-credential tests. These use only the test
  RPs `ctap-test*.example` and delete their credentials.
- `--set-pin`: on a key without a PIN, asks for a new one twice and sets it
  (only a FIDO reset removes it); refuses if a PIN is already set.
- `--destructive`: one wrong PIN (retries restored by the right PIN); 3 wrong
  in a row must give PIN_AUTH_BLOCKED even for the right PIN, then a power
  cycle and the right PIN restores the retries (needs >= 6 left; if the power
  cycle fails, enter the right PIN in the next run to restore them).
  Also the empty CBOR request (runs late): YubiKey 5.8 never answers and stays
  CHANNEL_BUSY, the test power-cycles it and fails (expected on YubiKey).
- `--reset` (asks to type RESET): authenticatorReset tests, run last. Make
  credentials, power cycle, reset at once with a touch (within 10 s of
  power-up: YubiKey, our firmware), check PIN and credentials are gone; then
  a reset 12 s after the first one must be refused (cancelled if the key asks
  for a touch instead). LionKey restarts its 10 s window after a reset.
- Hang guards: 5 s per HID report, 40 s per request (then CANCEL), 180 s per
  test (stack dump and exit). Run Python with `-u` when piping the output.
- Touches: one for the shared credential, one each for `exclude_list` and
  `assertion_with_touch`, one for the discoverable credential.

## Web site

Up when `web` says `running`; `web` prints the address(es).

- Default `web auto`: only while the device's own AP is up (`wifi mode ap` /
  `apsta`, or client mode's temporary AP). Join the AP (`esp32-auth`, default
  password `esp32-auth`: change it with `wifi ap`) and open http://192.168.4.1/.
- On a home network: `wifi set <ssid> <password>`, `web on`, then the IP from
  `web` / `wifi info`.
- Open site by default; `web login <user> <password>` locks every route.
- Quick API checks: `curl http://<ip>/api/status`, `/api/settings`,
  `/api/wifi/scan`.
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
| `hwinfo` | chip, MACs, eFuse unique ID, flash JEDEC/size, PSRAM, heap, eFuse security/download/USB bits, key blocks, NVS stats, flash layout (bootloader version, partitions, image sizes, gaps), OTA state (running/boot/next slot, rollback, raw otadata), CPU temperature, USB host, reset reason, uptime, clock (hardware RTC found or not, source, RTC timer, clock and system time to the ms, drift; no time: why, e.g. VL set), battery (as `battery`), SD card (as `sd`), LED (as `led info`) |
| `spiffs info` | partition address/size, mount point, total/used/free (esp_spiffs_info), file count |
| `spiffs ls` | SPIFFS files: size, modification time, file/fs totals |
| `spiffs cat <file>` | print a SPIFFS file as text |
| `spiffs catbase64 <file>` | print a SPIFFS file as base64, 76 chars per line |
| `spiffs write <file> [<length> <crc32>]` | prints `ready`, receives one base64 line (Enter ends the file), checks length/CRC32 (zlib), removes the file on error. Flow control: a `.` after every 192 base64 chars; send the next block only after it |
| `spiffs rm <file>` | delete a file |
| `spiffs format confirm` | erase the partition and create an empty fs (destroys stock data on the X4 Pro) |
| `ble on` / `ble off` | start / stop the BLE stack (off at boot) |
| `ble info` | state, address, advertising, pairing window, connection (MTU, security), bonds, keyboard/FIDO readiness |
| `ble pair [seconds]` | open the pairing window (default 60 s) and wait: prints `passkey nnnnnn`, returns `paired with <addr>` or an error when the window closes |
| `ble pair <seconds> bg` / `ble pair stop` | open the window and return (passkey only in the log) / close it |
| `ble bonds` | numbered bond list (address, type), marks the connected and the target host |
| `ble conns` | the connection (one at a time): peer (identity + over-the-air address), security, key size, MTU, interval/latency/timeout, RSSI, Service Changed subscription, stored CCCDs (handle:bits, `*` pending) |
| `ble use <#>` / `<addr>` / `any` | switch hosts: all keys released on the current host, link dropped, advertising filtered to the target bond (kept in NVS) so only it reconnects; `any` lifts the filter. A pairing window advertises to all; a newly paired host becomes the target |
| `ble disconnect` | drop the link (keys released first); the host may reconnect |
| `ble unpair <#>` / `<addr>` / `all` | delete a bond by number from `ble bonds`, by address, or all; a connected host is disconnected |
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
| `fido2 delete <#\|id>` | delete one: number from `fido2 list` (1-2 digits) or a unique id prefix (4+ hex digits; numbers shift after a delete) |
| `fido2 reset` / `fido2 reset confirm` | say what would go / delete every credential and the PIN (counter kept; the file becomes an empty store with the next version). Waits up to 1 s for a running FIDO request, else "busy". Also the way out of a refused store file |
| `sd [info]` / `sd ls [dir]` | microSD: card (CID name), size, bus clock, mount point, FAT free of total, or why not mounted / a directory: type, size, local modification time, name (no `..`). Read-only |
| `battery [info]` | gauge (CW2017 at 0x63, profile, last init action), voltage (mV), level (%), charging (STAT pin) |
| `battery regs` / `battery init` | gauge registers 0x00..0x0F and the resident profile, raw / check the profile again (loads it if it differs), reset the gauge |
| `board` | the board built for (`devices/config.h`): clock, display, touch, LED, battery, SD, frontlight, button and device pins |
| `led [info]` / `led <colour> [effect] [seconds]` | the board's WS2812: pin, UART, what it shows, ceilings, writes, task stack / show a colour (off white red green blue amber yellow cyan magenta) and effect (solid beacon slow blink fast breathe) for 1..600 s (default 5), then back |
| `led brightness [<n> [idle]]` | show / set the brightness ceilings in percent, normal and idle (saved) |
| `totp <base32 secret>` | local time (and UTC), seconds left in the window, the 6-digit codes of the previous, current and next 30 s window (HMAC-SHA1). Error without a believable time. The secret is not printed or logged, but stays in the up-arrow history |
| `totp test` | RFC 6238 SHA-1 vectors through `crypto::Totp` |
| `pass [info]` | password store: state (why it is off), records of max, file, version and change time |
| `pass list [filter]` | #, site / user, password set, TOTP; filter: part of site or user. Passwords are never shown |
| `pass add <site> [user] [password] [otp]` | a record; quote values with spaces. Prints its # (sorted by site, user) |
| `pass set <#> <site\|user\|password\|otp> [value]` | change a field, no value: clear. Prints the record's new # |
| `pass delete <#>` / `pass code <#>` | delete a record / its TOTP codes now |
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
| `config reload` / `config save` | re-read config.json (keeps values on error) and apply Wi-Fi (incl. internet check), web, time and LED settings / write current values |
| `efuse` | every eFuse field (list generated at build time from IDF `esp_efuse_table.csv`) and raw blocks BLK0..BLK10 |
