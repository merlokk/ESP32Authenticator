"""SPIFFS files on the device over the USB console.

  python utils/spiffs.py -p COM6 ls
  python utils/spiffs.py -p COM6 get <remote> [local]
  python utils/spiffs.py -p COM6 put <local> [remote]

Transfers are streamed in chunks on both sides; whole files are never held
in memory. `put` sends length and CRC32, and the device verifies them.
"""

import argparse
import base64
import binascii
import os
import re
import sys
import zlib

from device import EOL, Device

# Multiple of 3, so each chunk encodes to base64 without padding.
CHUNK = 3 * 1024
# Base64 chars per device ack, must match kAckBlock in components/cli/files.cpp.
ACK_BLOCK = 192
B64_LINE = re.compile(r"^[A-Za-z0-9+/]+={0,2}$")


def cmd_ls(dev: Device, args) -> int:
    for line in dev.run("spiffs ls"):
        print(line)
    return 0


def cmd_get(dev: Device, args) -> int:
    local = args.local or os.path.basename(args.remote)
    size = 0
    crc = 0
    dev.send_command(f"spiffs catbase64 {args.remote}")
    errors = []
    with open(local, "wb") as f:
        for line in dev.lines():
            if not B64_LINE.match(line):
                errors.append(line)
                continue
            data = binascii.a2b_base64(line)
            f.write(data)
            size += len(data)
            crc = zlib.crc32(data, crc)
    if errors:
        os.remove(local)
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"{args.remote} -> {local}: {size} bytes, crc32 {crc:08x}")
    return 0


def cmd_put(dev: Device, args) -> int:
    remote = args.remote or os.path.basename(args.local)
    size = os.path.getsize(args.local)
    crc = 0
    with open(args.local, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            crc = zlib.crc32(chunk, crc)

    dev.send_command(f"spiffs write {remote} {size} {crc:08x}")
    # The "ready" line usually arrives whole; wait for its end only if not.
    got = dev.read_until(b"ready")
    if b"\n" not in got.split(b"ready", 1)[1]:
        dev.read_until(b"\n")
    # Flow control (see spiffs write in components/cli/files.cpp): send one
    # ACK_BLOCK of base64 at a time and wait for the device's '.' after each.
    pending = b""
    with open(args.local, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            pending += base64.b64encode(chunk)
            while len(pending) >= ACK_BLOCK:
                dev.ser.write(pending[:ACK_BLOCK])
                pending = pending[ACK_BLOCK:]
                dev.read_until(b".")
    dev.ser.write(pending + EOL)

    result = [line for line in dev.lines(skip_echo=False) if line.strip(".")]
    print("\n".join(result))
    return 0 if any(line.startswith("ok ") for line in result) else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--port", required=True, help="serial port, e.g. COM6")
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ls", help="list files")
    p = sub.add_parser("get", help="read a file from the device")
    p.add_argument("remote")
    p.add_argument("local", nargs="?")
    p = sub.add_parser("put", help="write a file to the device")
    p.add_argument("local")
    p.add_argument("remote", nargs="?")
    args = parser.parse_args()

    with Device(args.port) as dev:
        return {"ls": cmd_ls, "get": cmd_get, "put": cmd_put}[args.cmd](dev, args)


if __name__ == "__main__":
    sys.exit(main())
