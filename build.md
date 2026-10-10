# Build, flash, CLI

## ESP-IDF

v6.0.2, installed by `eim` (manifest: `C:\Espressif\tools\eim_idf.json`). Stay on
6.0.x: the stock X4 Pro firmware is built with 6.0.1.
`idf.py` is a PowerShell function that exists only after sourcing the profile:

```powershell
pwsh -NoProfile -Command "& { . 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1' *> `$null; idf.py build }"
```

## Build and flash

```powershell
idf.py set-target esp32s3   # once
idf.py build
idf.py -p COM6 flash
```

- Dev board: bare ESP32-S3 (8 MB octal PSRAM, 16 MB flash) on USB Serial/JTAG,
  used until the X4 Pro is available.
- A board running a TinyUSB app (PID 4001) ignores DTR/RTS. Enter download
  mode by hand: hold BOOT, press RESET.
- If the app does not start after flashing, run
  `python -m esptool -p COM6 --before no-reset --after watchdog-reset chip-id`.
- On the X4 Pro the app `.bin` is flashed through the CrossPoint unlocker-tool
  (see [architecture.md](architecture.md)).

## Version

`version.txt` + git → `PROJECT_VER`, e.g. `0.1.0-10-g060dff6[-dirty]`
(`cmake/version.cmake`). It is computed at configure time: run
`idf.py reconfigure` to pick up a new commit.

## Utils

`utils/` (Python 3, `pip install -r utils/requirements.txt`): console helpers, see [utils/README.md](utils/README.md).
Transfers are streamed in chunks, files are never loaded whole. `device.py` answers smart-mode cursor queries and switches the console to `term dumb` on connect.

```powershell
python utils/spiffs.py -p COM6 ls
python utils/spiffs.py -p COM6 info
python utils/spiffs.py -p COM6 rm <remote>
python utils/spiffs.py -p COM6 get <remote> [local]
python utils/spiffs.py -p COM6 put <local> [remote]
python utils/ble.py -p COM6 on | off | info | bonds | conns | disconnect
python utils/ble.py -p COM6 pair [seconds]     # waits, prints the passkey
python utils/ble.py -p COM6 use <#|addr|any>
python utils/ble.py -p COM6 unpair <#|addr|all>
python utils/ble.py -p COM6 kb "text"          # or kb --stdin; quoting and splitting
                                                # into console lines is done for you
```

## Tests

```powershell
host_test\run.cmd [suite...]   # host tests, no board (MSVC); see architecture.md
```

`tests/fido2/`: CTAP2 authenticator suite (pytest + python-fido2,
`pip install -r tests/fido2/requirements.txt`). Transport-agnostic; checked
against a reference USB key (YubiKey) over `hid`, `ble` is next.
On Windows, raw FIDO HID access needs an administrator terminal.

```powershell
cd tests/fido2
python -m pytest [-k name] [--device YubiKey] [--pin 1234] [--set-pin] [--destructive] [--reset]
```

- Never changes the PIN; sets one only with `--set-pin`, resets only with `--reset`.
- PIN: asked at start when the key has one (or `--pin` / `FIDO_PIN`); empty
  skips the PIN, UV and discoverable-credential tests. These use only the test
  RPs `ctap-test*.example` and delete their credentials.
- `--set-pin`: on a key without a PIN, asks for a new one twice and sets it
  (only a FIDO reset removes it); refuses if a PIN is already set.
- `--destructive`: one wrong PIN (retries restored by the right PIN); 3 wrong
  in a row must give PIN_AUTH_BLOCKED even for the right PIN, then replug and
  the right PIN restores the retries (needs >= 6 left).
  Also the empty CBOR request (runs late): YubiKey 5.8 never answers and stays
  CHANNEL_BUSY, the test asks for a replug and fails (expected on YubiKey).
- `--reset` (asks to type RESET): authenticatorReset tests, run last. Make
  credentials, replug, reset at once with a touch (YubiKey: within 10 s of
  plug-in), check PIN and credentials are gone; then a reset 12 s after
  plug-in must be refused (cancelled if the key asks for a touch instead).
- Hang guards: 5 s per HID report, 40 s per request (then CANCEL), 180 s per
  test (stack dump and exit). Run Python with `-u` when piping the output.
- Touches: one for the shared credential, one each for `exclude_list` and
  `assertion_with_touch`, one for the discoverable credential.

## CLI

`esp_console` REPL on USB Serial/JTAG (same port as the monitor), prompt `auth>`.
Opening the port with RTS asserted resets the chip.

| Command | Output |
|---|---|
| `help` | command list |
| `term` / `term smart` / `term dumb` | ask the terminal / force line editing + up-arrow history (32 commands) on / off. Off at boot: nobody answers the probe then. Smart mode needs a terminal that answers cursor queries (PuTTY does); if the console goes silent, reset the board |
| `version` | firmware version, build date, IDF version, ELF SHA256, chip, running slot |
| `info` | short: model, MACs, unique ID, flash size, CPU temperature, reset reason, uptime |
| `hwinfo` | chip, MACs, eFuse unique ID, flash JEDEC/size, PSRAM, heap, eFuse security/download/USB bits, key blocks, NVS stats, flash layout (bootloader version, partitions, image sizes, gaps), OTA state (running/boot/next slot, rollback, raw otadata), CPU temperature, reset reason, uptime |
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
| `ble conns` | the connection (one at a time): peer (identity + over-the-air address), security, key size, MTU, interval/latency/timeout, RSSI |
| `ble use <#>` / `<addr>` / `any` | switch hosts: all keys released on the current host, link dropped, advertising filtered to the target bond (kept in NVS) so only it reconnects; `any` lifts the filter. A pairing window advertises to all; a newly paired host becomes the target |
| `ble disconnect` | drop the link (keys released first); the host may reconnect |
| `ble unpair <#>` / `<addr>` / `all` | delete a bond by number from `ble bonds`, by address, or all; a connected host is disconnected |
| `ble kb <text>` | type text on the connected host (US layout). esp_console drops single-backslash escapes: type `\\n` (Enter), `\\t` (Tab), `\\\\`; quote to keep spaces |
| `wifi on` / `wifi off` | start the station (tries the configured networks in order, retries every 2 s) / stop and unload the driver |
| `wifi info` | state, network being tried/joined, last error (wrong password / network not found / signal lost + code), MAC, hostname, BSSID, channel, RSSI, IP, netmask, gateway, DNS |
| `wifi scan` | access points: RSSI, channel, auth, SSID (radio must be on) |
| `wifi networks` | configured networks, in try order |
| `wifi set <ssid> [password]` / `wifi forget <ssid\|all>` | add/update / remove a network in config.json and save (quote an SSID with spaces; no password = open) |
| `config show` | current config as JSON, passwords masked |
| `config info` | file path, source (file / built-in defaults), why the file was not used |
| `config reload` / `config save` | re-read config.json (keeps values on error) and apply Wi-Fi settings / write current values |
| `efuse` | every eFuse field (list generated at build time from IDF `esp_efuse_table.csv`) and raw blocks BLK0..BLK10 |
