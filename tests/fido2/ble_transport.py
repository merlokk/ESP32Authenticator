"""FIDO over BLE client (CTAP 2.1 section 11.4) as a fido2 CtapDevice.

Frames go to fidoControlPoint (write with response) and come back as
fidoStatus notifications:
  initial:      cmd (0x80 | n), len hi, len lo, data
  continuation: seq (0..0x7F), data
Commands: PING 0x81, KEEPALIVE 0x82, MSG 0x83, CANCEL 0xBE, ERROR 0xBF.

bleak is asyncio; the device runs its own loop in a thread so that the
synchronous fido2 API works on top. On Windows the FIDO service is visible to
elevated processes only, and the device must be paired with the PC first.
"""

import asyncio
import re
import threading
import time

from fido2.ctap import CtapDevice, CtapError
from fido2.hid import CAPABILITY, CTAPHID

PING, KEEPALIVE, MSG, CANCEL, ERROR = 0x81, 0x82, 0x83, 0xBE, 0xBF
SERVICE = "0000fffd-0000-1000-8000-00805f9b34fb"
_U = "f1d0fff{}-deaa-ecee-b42f-c9ba7ed623bb"
CONTROL_POINT, STATUS, CONTROL_POINT_LENGTH, REVISION = (_U.format(i) for i in "1234")
REVISION_FIDO2 = 0x20

FRAME_TIMEOUT = 5.0  # max silence between frames (keepalives keep it alive)
CALL_DEADLINE = 40.0  # one request, including the wait for a touch
RUN_TIMEOUT = 60.0  # any single BLE operation (connect, write, read)
MAC = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")

# fido2 (CTAPHID) command -> BLE frame command
_CMD = {CTAPHID.CBOR: MSG, CTAPHID.MSG: MSG, CTAPHID.PING: PING}


class BleCtapDevice(CtapDevice):
    def __init__(self, address: str, name: str | None = None):
        from bleak import BleakClient

        self.address = address
        self.name = name
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._thread.start()
        self._frames: asyncio.Queue | None = None
        # Uncached GATT discovery: after the authenticator reboots, WinRT keeps
        # handing out the service list of the old link (no FIDO service).
        self._client = BleakClient(address, timeout=20, winrt={"use_cached_services": False})
        if MAC.match(address):
            # A bonded peripheral connected to the OS (e.g. as a keyboard) does
            # not advertise: give WinRT the address and skip bleak's scan.
            backend = self._client._backend
            if hasattr(backend, "_device_info"):
                backend._device_info = int(address.replace(":", ""), 16)
        try:
            self._run(self._connect())
        except BaseException:
            self._stop_loop()
            raise

    def __repr__(self):
        return f"BleCtapDevice({' '.join(filter(None, (self.name, self.address)))})"

    # --- sync <-> async

    def _run(self, coro, timeout=RUN_TIMEOUT):
        """Runs a coroutine on the device loop; never blocks forever."""
        if not self._loop.is_running():
            coro.close()
            raise ConnectionError("BLE device is closed")
        future = asyncio.run_coroutine_threadsafe(coro, self._loop)
        try:
            return future.result(timeout)
        except TimeoutError:
            future.cancel()
            raise TimeoutError(f"BLE operation took more than {timeout} s") from None

    async def _connect(self):
        self._frames = asyncio.Queue()
        await self._client.connect()
        if not any(s.uuid == SERVICE for s in self._client.services):
            await self._client.disconnect()
            raise RuntimeError("no FIDO service 0xFFFD: not paired, or (Windows) "
                               "not an administrator process")
        cpl = await self._client.read_gatt_char(CONTROL_POINT_LENGTH)
        self.control_point_length = int.from_bytes(cpl, "big")
        self.revision = (await self._client.read_gatt_char(REVISION))[0]
        if self.revision & REVISION_FIDO2:
            await self._client.write_gatt_char(REVISION, bytes([REVISION_FIDO2]),
                                               response=True)
        await self._client.start_notify(
            STATUS, lambda _, data: self._frames.put_nowait(bytes(data)))

    # --- frames

    @property
    def frame_size(self) -> int:
        """Max bytes per control point write: fidoControlPointLength, ATT MTU."""
        return min(self.control_point_length, self._client.mtu_size - 3)

    def write_frame(self, frame: bytes) -> None:
        self._run(self._client.write_gatt_char(CONTROL_POINT, frame, response=True))

    def read_frame(self, timeout: float = FRAME_TIMEOUT) -> bytes:
        """Next fidoStatus notification; TimeoutError if none comes in time."""
        try:
            return self._run(asyncio.wait_for(self._frames.get(), timeout))
        except (TimeoutError, asyncio.TimeoutError):
            raise TimeoutError(f"no fidoStatus frame in {timeout} s") from None

    def drain(self) -> list[bytes]:
        """Drops pending notifications (late answers), returns them."""
        async def take():
            out = []
            while not self._frames.empty():
                out.append(self._frames.get_nowait())
            return out
        return self._run(take())

    def fragment(self, cmd: int, data: bytes) -> list[bytes]:
        size = self.frame_size
        frames = [bytes([cmd, len(data) >> 8, len(data) & 0xFF]) + data[:size - 3]]
        rest, seq = data[size - 3:], 0
        while rest:
            frames.append(bytes([seq & 0x7F]) + rest[:size - 1])
            rest, seq = rest[size - 1:], seq + 1
        return frames

    def read_response(self, event=None, on_keepalive=None, deadline=CALL_DEADLINE):
        """Assembles one response: (cmd, payload). KEEPALIVE frames go to
        on_keepalive; ERROR comes back as cmd ERROR. When `event` is set,
        CANCEL is sent once and the answer to the request is still awaited."""
        end = time.monotonic() + deadline
        cancelled = False
        while True:
            if event is not None and event.is_set() and not cancelled:
                self.write_frame(bytes([CANCEL, 0, 0]))
                cancelled = True
            if time.monotonic() > end and not cancelled:
                if event is None:
                    event = threading.Event()
                event.set()
                continue
            frame = self.read_frame()
            cmd = frame[0]
            if cmd == KEEPALIVE:
                if on_keepalive and len(frame) >= 4:
                    on_keepalive(frame[3])
                continue
            if not cmd & 0x80 or len(frame) < 3:
                raise CtapError(CtapError.ERR.INVALID_SEQ)  # stray continuation
            length = (frame[1] << 8) | frame[2]
            data, seq = frame[3:], 0
            while len(data) < length:
                cont = self.read_frame()
                if cont[0] != seq:
                    raise CtapError(CtapError.ERR.INVALID_SEQ)
                data += cont[1:]
                seq = (seq + 1) & 0x7F
            return cmd, data[:length]

    # --- CtapDevice

    @property
    def capabilities(self) -> int:
        return CAPABILITY.CBOR

    def call(self, cmd, data=b"", event=None, on_keepalive=None) -> bytes:
        frame_cmd = _CMD.get(cmd, cmd)
        if not frame_cmd & 0x80:
            raise CtapError(CtapError.ERR.INVALID_COMMAND)
        self.drain()
        for frame in self.fragment(frame_cmd, bytes(data)):
            self.write_frame(frame)
        rcmd, payload = self.read_response(event, on_keepalive)
        if rcmd == ERROR:
            raise CtapError(payload[0] if payload else CtapError.ERR.OTHER)
        if rcmd != frame_cmd:
            raise CtapError(CtapError.ERR.INVALID_COMMAND)
        return payload

    def _stop_loop(self) -> None:
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(5)

    def close(self) -> None:
        if self._loop.is_running():
            try:
                self._run(self._client.disconnect(), timeout=10)
            except Exception:
                pass  # already gone
            finally:
                self._stop_loop()

    @classmethod
    def list_devices(cls):
        raise NotImplementedError("use open_ble()")


def open_ble(which: str | None, scan_time: float = 10.0) -> BleCtapDevice:
    """`which`: a MAC address (bonded device, no scan) or a name substring.
    Without an address, scans for devices advertising the FIDO service."""
    if which and MAC.match(which):
        return BleCtapDevice(which.upper())
    from bleak import BleakScanner

    async def scan():
        found = await BleakScanner.discover(scan_time, return_adv=True,
                                            service_uuids=[SERVICE])
        return [(d.address, a.local_name or d.name) for d, a in found.values()]

    found = asyncio.run(scan())
    if which:
        found = [f for f in found if which.lower() in (f[1] or "").lower()]
    if not found:
        raise LookupError("no BLE device advertises the FIDO service; a bonded "
                          "device connected to the OS does not advertise: "
                          "pass its address")
    if len(found) > 1:
        raise LookupError("several FIDO BLE devices: "
                          + ", ".join(f"{n} {a}" for a, n in found))
    return BleCtapDevice(*found[0])
