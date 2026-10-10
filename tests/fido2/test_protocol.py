"""Command dispatch and malformed requests."""

import threading

import pytest
from fido2.ctap import CtapError

import transports
from conftest import err_name

CBOR = 0x10  # CTAPHID.CBOR; other transports map it to their message command
ERR = CtapError.ERR


def status(device, payload: bytes) -> int:
    try:
        return device.call(CBOR, payload)[0]
    except CtapError as e:
        return e.code


def test_unknown_command(device):
    s = status(device, bytes([0x7F]))
    assert s == ERR.INVALID_COMMAND, err_name(s)


def test_invalid_cbor(device):
    """Truncated map. YubiKey answers CBOR_UNEXPECTED_TYPE here, not INVALID_CBOR."""
    s = status(device, bytes([0x01, 0xA1, 0x01]))  # makeCredential + truncated map
    assert s in (ERR.INVALID_CBOR, ERR.CBOR_UNEXPECTED_TYPE), err_name(s)


def test_cbor_not_a_map(device):
    s = status(device, bytes([0x01, 0x80]))  # makeCredential + empty array
    assert s in (ERR.CBOR_UNEXPECTED_TYPE, ERR.INVALID_CBOR), err_name(s)


def responsive(device, timeout=3.0) -> bool:
    """True if the device answers getInfo within `timeout` (CANCEL after)."""
    cancel = threading.Event()
    timer = threading.Timer(timeout, cancel.set)
    timer.start()
    try:
        return device.call(CBOR, bytes([0x04]), cancel)[0] == 0
    except (CtapError, TimeoutError):
        return False
    finally:
        timer.cancel()


@pytest.mark.destructive
@pytest.mark.replug
def test_empty_request(request, auth):
    """CBOR message without a command byte must be rejected.

    YubiKey 5.8 never answers it and stays CHANNEL_BUSY until replugged: then
    the test asks for a replug (so the run goes on) and fails.
    """
    try:
        s = status(auth.device, b"")
    except TimeoutError:
        s = None
    if s is None or not responsive(auth.device):
        auth.say("\n" + "=" * 60 +
                 "\n  The authenticator is wedged: unplug it and plug it back in."
                 "\n" + "=" * 60)
        auth.attach(transports.wait_replug(auth.device, request.config.getoption("--device")))
        pytest.fail("no answer to an empty CBOR request (got "
                    f"{'nothing' if s is None else err_name(s)}); wedged until replug")
    assert s in (ERR.INVALID_LENGTH, ERR.INVALID_COMMAND), err_name(s)


def test_still_works_after_errors(auth):
    assert auth.ctap2.get_info().versions
