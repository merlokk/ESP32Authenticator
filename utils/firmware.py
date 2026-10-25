"""Firmware updates over the USB console.

  python utils/firmware.py -p COM7 info                 # ota: slots, trial, firmware.bin
  python utils/firmware.py -p COM7 put [local]          # -> /sdcard/firmware.bin
  python utils/firmware.py -p COM7 install              # ota sd confirm: write it, restart
  python utils/firmware.py -p COM7 rm                   # delete /sdcard/firmware.bin

`local` defaults to build/esp32-authenticator.bin. Sent as base64 with length
and CRC32, as `spiffs.py put` (~100 KB/s: a 2 MB image takes ~30 s).
"""

import argparse
import os
import sys

from device import Device
from spiffs import run_and_print, send_file, size_and_crc

DEFAULT = os.path.join(os.path.dirname(__file__), "..", "build", "esp32-authenticator.bin")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--port", required=True, help="serial port, e.g. COM7")
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info", help="ota: slots, trial state, the card's firmware.bin")
    p = sub.add_parser("put", help="copy an app .bin to /sdcard/firmware.bin")
    p.add_argument("local", nargs="?", default=DEFAULT)
    sub.add_parser("install", help="write /sdcard/firmware.bin into the next slot, restart")
    sub.add_parser("rm", help="delete /sdcard/firmware.bin")
    args = parser.parse_args()

    with Device(args.port) as dev:
        if args.cmd == "info":
            return run_and_print(dev, "ota")
        if args.cmd == "install":
            return run_and_print(dev, "ota sd confirm")
        if args.cmd == "rm":
            return run_and_print(dev, "sd rm firmware.bin")
        size, crc = size_and_crc(args.local)
        print(f"{args.local} -> /sdcard/firmware.bin, {size} bytes")
        failed = send_file(dev, args.local, f"sd put firmware.bin {size} {crc:08x}")
        return failed or run_and_print(dev, "ota sd")


if __name__ == "__main__":
    sys.exit(main())
