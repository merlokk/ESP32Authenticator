# Testing on the device

What can be checked on the board itself: the console, the screen, the web
site, FIDO2 over BLE. Host tests (no board) are in [build.md](build.md#tests).

## Rules

- The board holds real data (FIDO2 credentials, PIN, BLE bonds, Wi-Fi
  passwords): `fido2 reset`, deleting non-test credentials, `ble unpair`,
  `spiffs format` only when the owner asks. Test leftovers
  (`ctap-test*.example`) may be removed.
- The owner types the PIN (pad or `pin`); scripts never do, and never tap the
  PIN pad's digits (`ui click`).
- Verify after every flash: console via `utils/`, HTTP via `curl`.

## Console

`utils/console.py` runs any command ([build.md](build.md#cli) has the list;
`help` on the device), see [utils/README.md](utils/README.md):

```powershell
python utils/console.py -p COM7 "fido2 info" "pass" "ui"   # several, in order
python utils/console.py -p COM7 --wait 30 "wifi ping"      # longer silence allowed
python utils/console.py -p COM7 devstatus                  # everything at once
python utils/console.py -p COM7 --log [seconds]            # read only: boot log, panics
```

- Console silent (commands time out, writes block): the app is hung or
  boot-looping. `--log` reads without writing; decode the backtrace with
  `xtensa-esp32s3-elf-addr2line -pfiaC -e build/esp32-authenticator.elf <addr...>`.
- The app does not start after flashing:
  `python -m esptool -p COM7 --before no-reset --after watchdog-reset chip-id`
  (inside the IDF profile).
- Stacks: `ui` prints the ui task's free stack (and the push task's); keep an
  eye on it after UI changes.

## config.json `test`

Development aids, saved in config.json (`config test`, or by hand). Never on
a device with real secrets; turn both off after a run.

| Key | Console | Effect |
|---|---|---|
| `test.pin` (empty or 6 digits) | `config test pin <6 digits>` / `config test off` | the vault (and FIDO2) unlock at boot: reboots in test runs need no PIN. Masked by `config show`, never on the site; a wrong one is logged |
| `test.autoPresence` (default false) | `config test presence on\|off` | FIDO2 / U2F user presence without the button (a log warning each time, `fido2 info` says AUTO; not the reset's 5 s hold). Read at every request: takes effect at once |

```json
"test": {"pin": "", "autoPresence": false}
```

## Board self-tests

| Command | Checks |
|---|---|
| `totp test` | RFC 6238 SHA-1 vectors through `crypto::Totp` (the HMAC; host tests cover the rest) |
| `epd test [orient\|corners\|edges\|checker\|black\|white\|grey] [full\|fast]` | the panel: which way is up, what the bezel hides, grey levels (UC8179) |
| `touch watch [s]` / `keys watch [s]` | raw GT911 frames / button events (pressed, released, long, click xN) |
| `led <colour> [effect] [s]`, `light <1..100>` | LED, frontlight |
| `battery`, `battery regs` | gauge, charger |
| `ui wallpaper [grey\|bw]` | a power-off wallpaper drawn now |
| `off wake <s>` | power-off and a timer wake (the kept PIN: `config power keep`) |
| `date sync`, `wifi ping` | time server, internet check |
| `ble kb "text"` (`utils/ble.py kb`) | typing on the connected host (US layout) |

## Screen

The screen can be driven and read from the console, without touching it.
Portrait coordinates: 480x800, (0, 0) top left; the status bar is the top
52 px, the tab row the bottom 72 px, a list row is cut in quarters of 118 px.

| Command | Does |
|---|---|
| `ui show list\|settings\|fido` | opens that screen, as its swipe would |
| `ui key left\|right\|home` | as the button (pages; Home: the list, page 1) |
| `ui click <x> <y> [ms]` | a made-up tap (down, ~60 ms or `ms` held, up) |
| `ui swipe <x0> <y0> <x1> <y1>` | a made-up finger moved in ~200 ms; classified as a real one (`nav::Classify`: a tap slides 24 px at most, a swipe goes 100 px, mostly along one axis; from the top quarter, y < 200, it opens Settings, from the bottom one, y >= 600, the FIDO2 list; sideways on a FIDO2 row, its trash can) |
| `ui screenshot` | LVGL's 1-bit screen as base64 with length and CRC32 |
| `ui demo fido [n]` / `ui demo off` | n (default 10, up to 30) made-up keys in the FIDO2 list, on the screen only (the store is not touched) |
| `ui` | refreshes, the last one, free stacks, touch (last point, presses) |

```powershell
python utils/screenshot.py -p COM7 [out.png]        # the screen as a PNG
python utils/console.py -p COM7 "ui swipe 240 780 240 400"   # the FIDO2 list
python utils/console.py -p COM7 "ui click 60 300"            # a row's first quarter: type the user
python utils/console.py -p COM7 "ui swipe 400 300 150 300"   # FIDO2: the trash can on the 2nd row
```

- A click or swipe is refused while a finger (real or made up) is down.
- The screenshot is what LVGL drew; the panel shows it about a second later.
- Under the PIN pad the screens do not take buttons or swipes.
- Real buttons: `keys watch` shows them.

## Test data

- Passwords: `pass add <site> [user] [password] [otp]` (made-up records,
  e.g. `pass add test1.example user1 pass1 JBSWY3DPEHPK3PXP`), `pass list`,
  `pass delete <#>`; the order: `pass move`, `pass sort`. Needs the open vault.
- FIDO2 keys on the screen: `ui demo fido`; deleting one from the screen
  (trash can, Yes held: `ui click <x> <y> 4500`) drops it from the screen
  only. Real test credentials only from
  the FIDO2 suite below (`ctap-test*.example`).

## Web site

Up when `web` says `running`; `web` prints the address.

```powershell
curl http://<ip>/api/status        # network, BLE, FIDO2 store, vault, SD, device
curl http://<ip>/api/settings
curl http://<ip>/api/wifi/scan
```

Pages and `app.js` are cached for an hour: reload with Ctrl+F5 after a flash.

## FIDO2 suite (`tests/fido2/`)

CTAP2 authenticator suite (pytest + python-fido2,
`pip install -r tests/fido2/requirements.txt`). Transport-agnostic, checked
against a reference USB key (YubiKey). On Windows both transports need an
administrator terminal (FIDO HID and the GATT FIDO service are hidden otherwise).

- `--transport hid` (default): USB CTAPHID.
- `--transport ble --device <address|name>`: FIDO over BLE (`ble_transport.py`,
  bleak). Pair the device with the PC first (`ble pair`, passkey in Windows
  Bluetooth settings). A bonded device already connected to the PC does not
  advertise: pass its address, the scan is skipped.
- Power cycle (replug, reset tests): on USB the user replugs the key; over BLE
  `--console COM7` sends `reboot` to our firmware, waits for `ble info` to say
  `state on` (the console drops commands right after boot), reconnects.
  Without `--console` these tests are skipped on BLE.
- `test_transport.py` (marker `transport`): PING on both transports, BLE
  framing errors. If getInfo fails (no CTAP2 core yet), only these run.

```powershell
cd tests/fido2
python -m pytest [-k name] [--transport ble] [--device YubiKey|<address>] [--console COM7] [--pin 1234] [--set-pin] [--destructive] [--fill] [--reset]
```

- Never changes the PIN; sets one only with `--set-pin`, resets only with `--reset`.
- Non-resident credentials of the test RPs (`ctap-test*.example`) can stay on
  our firmware after a run (U2F ones show as `U2F 3dec69ac`): check with `fido2 list`, remove with
  `fido2 delete <id>` (the store holds the board's `fido_max_credentials`; a run
  leaves ~6, a full store fails with KEY_STORE_FULL / U2F 0x6986).
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
- `--fill` (`test_fill.py`, runs late): discoverable credentials for
  `ctap-test-fill.example` until KEY_STORE_FULL; the count must match
  getCredsMetadata's remaining, then all are deleted. One touch each: on our
  firmware `config test presence on` first (and `off` after). With reboots
  (`--console`) on a PIN-locked store also `config test pin <PIN>` (and `off`).
- `--reset` (asks to type RESET): authenticatorReset tests, run last. Make
  credentials, power cycle, reset at once with a touch (within 10 s of
  power-up: YubiKey, our firmware), check PIN and credentials are gone; then
  a reset 12 s after the first one must be refused (cancelled if the key asks
  for a touch instead). LionKey restarts its 10 s window after a reset.
- Elevated from a normal shell (the elevated window cannot run a script
  from Claude's temp folder): `Start-Process pwsh -Verb RunAs -ArgumentList
  '-NoProfile','-Command','& <script in the repo>'`.
- X4 Pro over BLE (2026-10-23, `--console COM7 --destructive --fill`): 103
  passed, 5 skipped (`--reset`); the store filled at 125 credentials.
- Hang guards: 5 s per HID report, 40 s per request (then CANCEL), 180 s per
  test (stack dump and exit). Run Python with `-u` when piping the output.
- Touches: one for the shared credential, one each for `exclude_list` and
  `assertion_with_touch`, one for the discoverable credential.
