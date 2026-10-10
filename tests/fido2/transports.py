"""Transports: each returns a fido2 CtapDevice.

hid: USB CTAPHID (reference keys such as YubiKey).
ble: FIDO over BLE (CTAP 2.1 section 11.4), see ble_transport.py.
On Windows both need an elevated (administrator) process.
"""

import sys
import threading

import pytest

NAMES = ["hid", "ble"]

# Max wait for one HID report. While waiting for a touch the authenticator
# sends KEEPALIVE every ~100 ms, so only a silent device hits this.
READ_TIMEOUT = 5.0


class TimeoutConnection:
    """Wraps a fido2 HID connection: read_packet raises TimeoutError instead of
    blocking forever when the device sends nothing (Windows only).

    After a timeout the handle is reopened, so a late answer to the abandoned
    request does not end up as the answer to the next one.
    """

    def __init__(self, descriptor, timeout=READ_TIMEOUT):
        from fido2.hid import open_connection

        self._open = lambda: open_connection(descriptor)
        self._conn = self._open()
        self.timeout = timeout

    def write_packet(self, data):
        self._conn.write_packet(data)

    def close(self):
        self._conn.close()

    def read_packet(self):
        if sys.platform != "win32":
            return self._conn.read_packet()
        import ctypes

        k32 = ctypes.windll.kernel32
        k32.OpenThread.restype = ctypes.c_void_p
        k32.CancelSynchronousIo.argtypes = [ctypes.c_void_p]
        k32.CloseHandle.argtypes = [ctypes.c_void_p]
        THREAD_TERMINATE = 0x0001  # access right CancelSynchronousIo needs
        thread = k32.OpenThread(THREAD_TERMINATE, False, threading.get_native_id())
        lock = threading.Lock()
        state = {"reading": True, "fired": False}

        def cancel():
            with lock:
                if state["reading"]:
                    state["fired"] = True
                    k32.CancelSynchronousIo(thread)

        timer = threading.Timer(self.timeout, cancel)
        timer.start()
        try:
            return self._conn.read_packet()
        except OSError:
            if state["fired"]:
                self._conn.close()
                self._conn = self._open()
                raise TimeoutError(f"no HID report from the device in {self.timeout} s")
            raise
        finally:
            with lock:
                state["reading"] = False
            timer.cancel()
            k32.CloseHandle(thread)


CALL_DEADLINE = 40.0  # one request, including the wait for a touch


def _with_deadline(cls):
    """Subclass of a CtapDevice whose call() gives up after CALL_DEADLINE.

    The cancel event makes fido2 send CANCEL and stops its endless retry on
    CHANNEL_BUSY; the request then fails instead of hanging.
    """

    class Deadline(cls):
        def call(self, cmd, data=b"", event=None, on_keepalive=None):
            event = event or threading.Event()
            timer = threading.Timer(CALL_DEADLINE, event.set)
            timer.start()
            try:
                return super().call(cmd, data, event, on_keepalive)
            finally:
                timer.cancel()

    return Deadline


def _open_hid(name_filter):
    from fido2.hid import CtapHidDevice, list_descriptors

    descs = list(list_descriptors())
    if name_filter:
        descs = [d for d in descs
                 if name_filter.lower() in (d.product_name or "").lower()]
    if not descs:
        hint = (" (Windows: run from an administrator terminal)"
                if sys.platform == "win32" else "")
        pytest.exit("no FIDO HID device found" + hint, returncode=2)
    if len(descs) > 1:
        names = ", ".join(str(d.product_name) for d in descs)
        pytest.exit(f"several FIDO HID devices ({names}): use --device", returncode=2)
    try:
        return _with_deadline(CtapHidDevice)(descs[0], TimeoutConnection(descs[0]))
    except TimeoutError:
        pytest.exit("the device does not answer CTAPHID INIT: replug it", returncode=2)


def _hid_descriptors(name_filter):
    from fido2.hid import list_descriptors

    return [d for d in list_descriptors()
            if not name_filter or name_filter.lower() in (d.product_name or "").lower()]


def _replug_hid(device, name_filter, say, timeout):
    import time

    from fido2.hid import CtapHidDevice

    say(">>> unplug the authenticator and plug it back in")
    path = device.descriptor.path
    device.close()
    deadline = time.monotonic() + timeout
    while any(d.path == path for d in _hid_descriptors(name_filter)):
        if time.monotonic() > deadline:
            raise TimeoutError("the device was not unplugged")
        time.sleep(0.1)
    while True:
        descs = _hid_descriptors(name_filter)
        if len(descs) == 1:
            try:
                return _with_deadline(CtapHidDevice)(descs[0], TimeoutConnection(descs[0]))
            except (OSError, TimeoutError):
                pass  # just enumerated, not ready yet
        if time.monotonic() > deadline:
            raise TimeoutError("the device did not come back")
        time.sleep(0.05)


def _replug_ble(device, console_port, say, timeout):
    """Power cycle of our firmware: `reboot` on the USB console, `ble on`, and
    a new BLE connection (the bonded host reconnects to the advertising)."""
    import time
    from pathlib import Path

    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "utils"))
    from device import Device

    from ble_transport import BleCtapDevice

    say(f">>> rebooting the authenticator over {console_port}")
    address = device.address
    device.close()
    with Device(console_port) as console:
        console.send_command("reboot")
    start = time.monotonic()
    time.sleep(1.0)  # the USB console drops while the chip restarts
    deadline = start + timeout
    last = None
    while True:
        # Right after boot the console may drop a command (the REPL is not up
        # yet): check `ble info` until BLE is really on.
        try:
            with Device(console_port, timeout=3) as console:
                console.run("ble on")  # an error if BLE autostarted: ignored
                info = console.run("ble info")
            if any(line.split()[:2] == ["state", "on"] for line in info if line.strip()):
                break
            last = f"ble info: {info[:1]}"
        except Exception as e:
            last = e
        if time.monotonic() > deadline:
            raise TimeoutError(f"BLE did not come back on: {last!r}") from None
        time.sleep(0.3)
    say(f"    console back after {time.monotonic() - start:.1f} s, BLE on")
    while True:
        try:
            dev = BleCtapDevice(address)
            say(f"    BLE connected after {time.monotonic() - start:.1f} s")
            return dev
        except Exception as e:
            say(f"    BLE connect failed: {e!r}")
            if time.monotonic() > deadline:
                raise TimeoutError(f"BLE did not reconnect: {e!r}") from None
            time.sleep(0.5)


def can_replug(config) -> bool:
    return config.getoption("--transport") == "hid" or bool(config.getoption("--console"))


def wait_replug(device, config, say, timeout=60.0):
    """Power-cycles the authenticator and returns it reopened: on USB the user
    replugs it, over BLE (needs --console) the firmware reboots."""
    if config.getoption("--transport") == "hid":
        return _replug_hid(device, config.getoption("--device"), say, timeout)
    return _replug_ble(device, config.getoption("--console"), say, timeout)


def _open_ble(which):
    from ble_transport import open_ble

    try:
        return open_ble(which)
    except (LookupError, RuntimeError) as e:
        hint = (" (Windows: run from an administrator terminal)"
                if sys.platform == "win32" else "")
        pytest.exit(f"BLE: {e}{hint}", returncode=2)


def open_device(transport, name_filter=None):
    if transport == "hid":
        return _open_hid(name_filter)
    return _open_ble(name_filter)
