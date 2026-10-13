"""Any console command, or the raw log of the device.

  python utils/console.py -p COM6 "wifi info" "web"     run commands in order
  python utils/console.py -p COM6 --wait 10 "wifi ping"  wait longer for output
  python utils/console.py -p COM6 --log [seconds]       only read the port (default 10 s)

`--log` never writes: use it when commands time out (a hung or boot-looping
app does not read its console). It prints the boot log and any panic
backtrace; decode the addresses with xtensa-esp32s3-elf-addr2line (build.md).

Exit code 1 when the device reports an error.
"""

import argparse
import sys
import time

import serial

from device import Device


def read_log(port: str, seconds: float) -> int:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False  # RTS asserted would reset the chip
    ser.rts = False
    ser.open()
    try:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            data = ser.read(4096)
            if data:
                sys.stdout.write(data.decode(errors="replace").replace("\r", ""))
                sys.stdout.flush()
    finally:
        ser.close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--port", required=True, help="serial port, e.g. COM6")
    parser.add_argument("--wait", type=float, default=10.0,
                        help="seconds of silence before a command counts as stuck")
    parser.add_argument("--log", nargs="?", type=float, const=10.0, metavar="SECONDS",
                        help="only read the port for SECONDS (no commands sent)")
    parser.add_argument("commands", nargs="*", help="console commands, run in order")
    args = parser.parse_args()

    if args.log is not None:
        return read_log(args.port, args.log)
    if not args.commands:
        parser.error("give commands, or --log")

    failed = False
    with Device(args.port, timeout=args.wait) as dev:
        for command in args.commands:
            if len(args.commands) > 1:
                print(f">>> {command}", flush=True)
            dev.send_command(command)
            for line in dev.lines():
                print(line, flush=True)
                failed = failed or "error code" in line
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
