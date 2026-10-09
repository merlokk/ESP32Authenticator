# utils

Host-side Python tools that drive the device over its USB console (USB Serial/JTAG).

## Setup

```powershell
pip install -r utils/requirements.txt   # pyserial
```

All tools take `-p <port>` (e.g. `COM6`) and exit with 1 when the device reports an error.

| File | Purpose |
|---|---|
| `device.py` | Console session used by the tools: opens the port with DTR/RTS released (RTS resets the chip), answers smart-mode cursor queries, switches the console to `term dumb`, streams command output line by line |
| `spiffs.py` | SPIFFS files |
| `ble.py` | BLE stack, pairing, host switching, keyboard |

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

## ble.py

```powershell
python utils/ble.py -p COM6 on | off | info | bonds | conns | disconnect
python utils/ble.py -p COM6 pair [seconds]
python utils/ble.py -p COM6 use <#|addr|any>
python utils/ble.py -p COM6 unpair <#|addr|all>
python utils/ble.py -p COM6 kb "text"
python utils/ble.py -p COM6 kb --stdin < file.txt
```

- `pair`: opens the pairing window and waits; prints `passkey nnnnnn` (enter it on the host) and `paired with <addr>`.
- `use`: switch hosts (`#` from `bonds`); keys are released on the old host first.
- `kb`: types text on the connected host (US layout, ASCII). Quoting and escaping for the console are done here, and long text is split into several commands (console line limit 256).

## Notes

- One tool at a time per port; close PuTTY first.
- After a tool run the console is in `term dumb`; type `term smart` again in PuTTY for history.
