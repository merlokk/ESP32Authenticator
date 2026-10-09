"""Serial console session with the device (esp_console REPL, prompt `auth>`).

The port is opened with DTR/RTS released: on USB Serial/JTAG an asserted RTS
resets the chip.
"""

import re
import time

import serial

PROMPT = b"auth> "
# The console treats both CR and LF as end of line: CRLF would run an extra
# empty command and print a second prompt.
EOL = b"\r"
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
LOG_LINE = re.compile(r"^[IWEDV] \(\d+\) ")


class Device:
    def __init__(self, port: str, timeout: float = 10.0):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.1
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.timeout = timeout
        self.sync()

    def close(self) -> None:
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def sync(self) -> None:
        """Gets a fresh prompt, dropping whatever was pending."""
        self.ser.reset_input_buffer()
        self.ser.write(EOL)
        self.read_until(PROMPT)
        self.ser.reset_input_buffer()

    def read_until(self, marker: bytes, timeout: float | None = None) -> bytes:
        deadline = time.monotonic() + (timeout or self.timeout)
        data = b""
        while marker not in data:
            if time.monotonic() > deadline:
                raise TimeoutError(f"no {marker!r} from device; got {data[-200:]!r}")
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                data += chunk
                deadline = time.monotonic() + (timeout or self.timeout)
        return data

    def send_command(self, line: str) -> None:
        self.ser.write(line.encode() + EOL)

    def lines(self, skip_echo: bool = True):
        """Yields output lines of the running command until the prompt.

        Streams: nothing is accumulated beyond one line. Skips the command echo
        and ESP log lines.
        """
        buf = b""
        first = skip_echo
        deadline = time.monotonic() + self.timeout
        while True:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                buf += chunk
                deadline = time.monotonic() + self.timeout
            elif time.monotonic() > deadline:
                raise TimeoutError("device stopped answering")
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = ANSI.sub(b"", raw).decode(errors="replace").rstrip("\r")
                if first:  # echo of the command line
                    first = False
                    continue
                if LOG_LINE.match(line):
                    continue
                yield line
            if buf.endswith(PROMPT) or ANSI.sub(b"", buf).endswith(PROMPT):
                return

    def run(self, line: str) -> list[str]:
        self.send_command(line)
        return list(self.lines())
