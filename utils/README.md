# utils

Host-side Python tools that drive the device over its USB console (USB Serial/JTAG).

## Setup

```powershell
pip install -r utils/requirements.txt   # pyserial
```

All tools take `-p <port>` (e.g. `COM6`) and exit with 1 when the device reports an error.

| File | Purpose |
|---|---|
| `device.py` | Console session used by the tools: opens the port with DTR/RTS released (RTS resets the chip), answers smart-mode cursor queries, switches the console to `term dumb`, streams command output line by line. Writes time out after 2 s (a hung app does not read its console) |
| `console.py` | Any console command (`wifi`, `web`, `config`, `devstatus`, ...), or the raw log without writing |
| `spiffs.py` | SPIFFS files |
| `wallpaper.py` | power-off wallpapers (PNG) in `/sdcard/wallpapers` |
| `firmware.py` | firmware update: `ota` info, an app `.bin` to `/sdcard/firmware.bin`, `ota sd confirm`, remove it |
| `ble.py` | BLE stack, pairing, host switching, keyboard |
| `screenshot.py` | the e-ink screen as a PNG (`ui screenshot`) |

## console.py

```powershell
python utils/console.py -p COM6 "wifi info" "web"       # several commands, in order
python utils/console.py -p COM6 --wait 30 "wifi ping"   # longer silence allowed per command
python utils/console.py -p COM6 --log [seconds]         # read only, default 10 s
python utils/console.py -p COM6 devstatus               # everything at once
python utils/console.py -p COM6 "fido2 list"            # credentials: #, id, site, user
python utils/console.py -p COM6 "fido2 delete 905f88d7" # by id prefix (numbers shift)
python utils/console.py -p COM6 "wifi static 1 192.168.1.50 255.255.255.0 192.168.1.1"
```

The command list is in [build.md](../build.md#cli) (`help` on the device).

- Prints each command's output (ESP log lines filtered); with several commands
  each starts with `>>> <command>`. Exit code 1 if any command failed; the
  rest still run.
- Several deletes: use ids, not `#` (the list renumbers after each one).
- `--log`: when commands time out ("the device does not read its console"),
  read the boot log and panic backtrace; decode the addresses with
  `xtensa-esp32s3-elf-addr2line` (see [build.md](../build.md)).
- Secrets typed as arguments (`wifi set <ssid> <password>`) stay in the shell
  history; type them yourself.

## spiffs.py

```powershell
python utils/spiffs.py -p COM6 ls
python utils/spiffs.py -p COM6 info
python utils/spiffs.py -p COM6 get <remote> [local]
python utils/spiffs.py -p COM6 put <local> [remote]
python utils/spiffs.py -p COM6 rm <remote>
```

- `get`: `spiffs catbase64`, decoded line by line.
- `put`: `spiffs write <file> <length> <crc32>`; sends base64 in 192-char blocks and waits for the device's `.` ack after each (its RX buffer is 256 bytes and drops overflow). The device checks length and CRC32 (zlib) and removes the file on mismatch.
- Files are streamed in chunks on both sides, never loaded whole.

## wallpaper.py

```powershell
python utils/wallpaper.py -p COM6 put assets/*.png   # globs expanded here (cmd.exe does not)
python utils/wallpaper.py -p COM6 ls
python utils/wallpaper.py -p COM6 rm <name.png>
```

- `put`: `sd put <name> <length> <crc32>`, the same base64 stream as `spiffs.py put` (~100 KB/s). Plain `*.png` names without spaces; the device turns them to 1 bit itself.

## ble.py

```powershell
python utils/ble.py -p COM6                  # device usage, then info
python utils/ble.py -p COM6 on | off | info | bonds | conns | disconnect
python utils/ble.py -p COM6 pair [seconds] [--bg] | pair stop
python utils/ble.py -p COM6 use <#|addr|any>
python utils/ble.py -p COM6 unpair <#|addr|all>
python utils/ble.py -p COM6 kb "text"
python utils/ble.py -p COM6 kb --stdin < file.txt
```

- `pair`: opens the pairing window and waits; prints `passkey nnnnnn` (enter it on the host) and `paired with <addr>`.
- `use`: switch hosts (`#` from `bonds`); keys are released on the old host first.
- `kb`: types text on the connected host (US layout, ASCII). Quoting and escaping for the console are done here, and long text is split into several commands (console line limit 256).

## screenshot.py

```powershell
python utils/screenshot.py -p COM7 [out.png]   # default screen-<time>.png
```

- `ui screenshot`: LVGL's 1-bit portrait screen (480x800) as base64 with
  length and CRC32; written as a 1-bit greyscale PNG (no Pillow). Driving the
  screen (`ui show`, `ui key`, `ui click`, `ui swipe`): [testing-on-device.md](../testing-on-device.md#screen).

## Notes

- One tool at a time per port; close PuTTY first.
- After a tool run the console is in `term dumb`; type `term smart` again in PuTTY for history.
