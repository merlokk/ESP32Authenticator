"""The e-ink screen as a PNG, over the USB console.

  python utils/screenshot.py -p COM7 [out.png]

`ui screenshot` prints LVGL's 1-bit portrait screen (480x800, 1 white, MSB
the leftmost pixel) as base64 with its length and CRC32: what is drawn, or
about to be (the panel takes a second a frame). The PNG is 1-bit greyscale,
the same bits; written without Pillow. Default name: screen-<time>.png.
"""

import argparse
import binascii
import re
import struct
import sys
import time
import zlib

from device import Device

HEAD = re.compile(r"^screenshot (\d+) (\d+) 1bit$")
END = re.compile(r"^end (\d+) bytes, crc32 ([0-9a-f]{8})$")
B64_LINE = re.compile(r"^[A-Za-z0-9+/]+={0,2}$")


def png(width: int, height: int, bits: bytes) -> bytes:
    row = width // 8
    raw = b"".join(b"\x00" + bits[y * row:(y + 1) * row] for y in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data)))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--port", required=True, help="serial port, e.g. COM7")
    parser.add_argument("out", nargs="?", help="PNG file (default screen-<time>.png)")
    args = parser.parse_args()
    out = args.out or time.strftime("screen-%Y%m%d-%H%M%S.png")

    size = None
    end = None
    data = bytearray()
    other = []
    with Device(args.port) as dev:
        dev.send_command("ui screenshot")
        for line in dev.lines():
            if m := HEAD.match(line):
                size = int(m[1]), int(m[2])
            elif m := END.match(line):
                end = int(m[1]), int(m[2], 16)
            elif size and not end and B64_LINE.match(line):
                data += binascii.a2b_base64(line)
            else:
                other.append(line)
    if size is None or end is None:
        print("\n".join(other) or "no screenshot", file=sys.stderr)
        return 1
    width, height = size
    if len(data) != end[0] or zlib.crc32(data) != end[1] or len(data) != width * height // 8:
        print(f"broken: {len(data)} bytes, crc32 {zlib.crc32(data):08x}, "
              f"want {end[0]} / {end[1]:08x}", file=sys.stderr)
        return 1
    with open(out, "wb") as f:
        f.write(png(width, height, bytes(data)))
    print(f"{out}: {width}x{height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
