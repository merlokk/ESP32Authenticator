"""Transports: each returns a fido2 CtapDevice.

hid: USB CTAPHID (reference keys such as YubiKey). On Windows raw FIDO HID
     access needs an elevated (administrator) process.
ble: FIDO over BLE (CTAP 2.1 section 11.4), not implemented yet.
"""

import sys

import pytest

NAMES = ["hid", "ble"]


def _open_hid(name_filter):
    from fido2.hid import CtapHidDevice

    devs = list(CtapHidDevice.list_devices())
    if name_filter:
        devs = [d for d in devs
                if name_filter.lower() in (d.descriptor.product_name or "").lower()]
    if not devs:
        hint = (" (Windows: run from an administrator terminal)"
                if sys.platform == "win32" else "")
        pytest.exit("no FIDO HID device found" + hint, returncode=2)
    if len(devs) > 1:
        names = ", ".join(str(d.descriptor.product_name) for d in devs)
        pytest.exit(f"several FIDO HID devices ({names}): use --device", returncode=2)
    return devs[0]


def open_device(transport, name_filter=None):
    if transport == "hid":
        return _open_hid(name_filter)
    pytest.exit(f"transport {transport} is not implemented yet", returncode=2)
