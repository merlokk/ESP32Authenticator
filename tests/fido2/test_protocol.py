"""Command dispatch and malformed requests."""

import pytest
from fido2.ctap import CtapError

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


@pytest.mark.destructive
def test_empty_request(device):
    """CBOR message without a command byte.

    YubiKey 5.8 never answers it and stays CHANNEL_BUSY until replugged.
    """
    s = status(device, b"")
    assert s in (ERR.INVALID_LENGTH, ERR.INVALID_COMMAND), err_name(s)


def test_still_works_after_errors(auth):
    assert auth.ctap2.get_info().versions
