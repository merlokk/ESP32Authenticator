"""Power-off wallpapers in /sdcard/wallpapers over the USB console.

  python utils/wallpaper.py -p COM7 ls
  python utils/wallpaper.py -p COM7 put assets/*.png
  python utils/wallpaper.py -p COM7 rm <name.png>

PNG, portrait; 480x800 fills the screen (other sizes are centred on white or
cropped). The device turns it to 1 bit itself (Atkinson dither). Sent as
base64 with length and CRC32, as `spiffs.py put` (~100 KB/s).
"""

import argparse
import glob
import os
import sys

from device import Device
from spiffs import run_and_print, send_file, size_and_crc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--port", required=True, help="serial port, e.g. COM7")
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ls", help="list the wallpapers")
    p = sub.add_parser("put", help="copy PNGs to the card")
    p.add_argument("local", nargs="+")
    p = sub.add_parser("rm", help="delete a wallpaper")
    p.add_argument("name")
    args = parser.parse_args()

    with Device(args.port) as dev:
        if args.cmd == "ls":
            return run_and_print(dev, "sd ls wallpapers")
        if args.cmd == "rm":
            return run_and_print(dev, f"sd rm {args.name}")
        failed = 0
        for pattern in args.local:  # cmd.exe does not expand *.png
            for local in sorted(glob.glob(pattern)) or [pattern]:
                name = os.path.basename(local)
                if " " in name:
                    print(f"{name}: no spaces in the name (one console argument)", file=sys.stderr)
                    failed += 1
                    continue
                size, crc = size_and_crc(local)
                print(f"{local} -> wallpapers/{name}, {size} bytes")
                failed += send_file(dev, local, f"sd put {name} {size} {crc:08x}")
        return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
