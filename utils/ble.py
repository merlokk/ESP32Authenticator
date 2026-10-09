"""BLE control over the USB console.

  python utils/ble.py -p COM6 on | off | info | bonds | conns | disconnect
  python utils/ble.py -p COM6 pair [seconds]        waits, prints the passkey
  python utils/ble.py -p COM6 use <#|addr|any>      switch the target host
  python utils/ble.py -p COM6 unpair <#|addr|all>
  python utils/ble.py -p COM6 kb <text...>          type text (US layout)
  python utils/ble.py -p COM6 kb --stdin            type text read from stdin

Exit code 1 when the device reports an error.
"""

import argparse
import sys

from device import Device

# Console line limit is 256; leave room for `ble kb "..."` and escapes.
KB_CHUNK = 200


def run(dev: Device, command: str) -> int:
    """Runs a console command, printing its output as it arrives."""
    dev.send_command(command)
    failed = False
    for line in dev.lines():
        print(line, flush=True)
        failed = failed or "error code" in line
    return 1 if failed else 0


def kb_arg(text: str) -> str:
    """Quotes `text` for `ble kb`. esp_console drops unknown escapes, and the
    command expands \\n, \\t and \\\\ itself, so backslashes are doubled once
    for the console and once for the command."""
    out = []
    for c in text:
        if c == "\n":
            out.append("\\\\n")
        elif c == "\t":
            out.append("\\\\t")
        elif c == "\\":
            out.append("\\\\\\\\")
        elif c == '"':
            out.append('\\"')
        else:
            out.append(c)
    return '"' + "".join(out) + '"'


def kb_chunks(text: str):
    """Splits text so each quoted chunk fits the console line."""
    chunk = ""
    for c in text:
        if len(kb_arg(chunk + c)) > KB_CHUNK:
            yield chunk
            chunk = ""
        chunk += c
    if chunk:
        yield chunk


def cmd_kb(dev: Device, args) -> int:
    text = sys.stdin.read() if args.stdin else " ".join(args.text)
    if not text:
        print("nothing to type", file=sys.stderr)
        return 1
    for chunk in kb_chunks(text):
        if run(dev, f"ble kb {kb_arg(chunk)}") != 0:
            return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--port", required=True, help="serial port, e.g. COM6")
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name, help_text in (("on", "start the BLE stack"), ("off", "stop it"),
                            ("info", "state summary"), ("bonds", "numbered bond list"),
                            ("conns", "the connection"), ("disconnect", "drop the link")):
        sub.add_parser(name, help=help_text)
    p = sub.add_parser("pair", help="open the pairing window and wait for a new bond")
    p.add_argument("seconds", nargs="?", type=int, default=60)
    p = sub.add_parser("use", help="switch the target host")
    p.add_argument("which", help="# from 'bonds', address, or 'any'")
    p = sub.add_parser("unpair", help="delete a bond")
    p.add_argument("which", help="# from 'bonds', address, or 'all'")
    p = sub.add_parser("kb", help="type text on the connected host")
    p.add_argument("text", nargs="*")
    p.add_argument("--stdin", action="store_true", help="read the text from stdin")
    args = parser.parse_args()

    with Device(args.port) as dev:
        if args.cmd == "kb":
            return cmd_kb(dev, args)
        if args.cmd == "pair":
            dev.timeout = args.seconds + 10  # silent while waiting for the host
            return run(dev, f"ble pair {args.seconds}")
        if args.cmd in ("use", "unpair"):
            return run(dev, f"ble {args.cmd} {args.which}")
        return run(dev, f"ble {args.cmd}")


if __name__ == "__main__":
    sys.exit(main())
