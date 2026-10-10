"""Transport level: PING on every transport, BLE framing (CTAP 2.1 11.4).

Runs without a CTAP2 core on the authenticator.
"""

import os

import pytest
from fido2.ctap import CtapError
from fido2.hid import CTAPHID

from conftest import err_name

pytestmark = pytest.mark.transport
ERR = CtapError.ERR


@pytest.mark.parametrize("size", [0, 1, 57, 100, 600, 1000])
def test_ping_echo(device, size):
    """PING echoes its payload; 600/1000 span several frames on BLE and HID."""
    data = os.urandom(size)
    assert bytes(device.call(CTAPHID.PING, data)) == data


def test_ping_after_ping(device):
    for _ in range(5):
        assert bytes(device.call(CTAPHID.PING, b"again")) == b"again"


# --- BLE framing

@pytest.fixture
def ble(device):
    from ble_transport import BleCtapDevice

    if not isinstance(device, BleCtapDevice):
        pytest.skip("BLE only")
    device.drain()
    yield device
    device.drain()


def ble_error(ble, frames):
    """Writes raw frames, returns the ERROR code of the answer."""
    from ble_transport import ERROR

    for f in frames:
        ble.write_frame(f)
    cmd, payload = ble.read_response(deadline=5)
    assert cmd == ERROR, f"want ERROR, got cmd {cmd:#x} {payload.hex()}"
    return payload[0]


def test_control_point_length(ble):
    assert 20 <= ble.control_point_length <= 512


def test_revision_fido2(ble):
    from ble_transport import REVISION_FIDO2

    assert ble.revision & REVISION_FIDO2


def test_unknown_command(ble):
    code = ble_error(ble, [bytes([0x84, 0, 0])])
    assert code == ERR.INVALID_COMMAND, err_name(code)


def test_continuation_without_init(ble):
    code = ble_error(ble, [bytes([0x00]) + b"data"])
    assert code == ERR.INVALID_SEQ, err_name(code)


def test_wrong_sequence(ble):
    from ble_transport import PING

    size = ble.frame_size
    first = bytes([PING, 0x02, 0x00]) + bytes(size - 3)  # 512-byte PING
    code = ble_error(ble, [first, bytes([1]) + bytes(size - 1)])  # seq 1, not 0
    assert code == ERR.INVALID_SEQ, err_name(code)


def test_more_data_than_announced(ble):
    from ble_transport import PING

    code = ble_error(ble, [bytes([PING, 0, 2]) + b"12345"])
    assert code == ERR.INVALID_LENGTH, err_name(code)


def test_short_initial_frame(ble):
    from ble_transport import PING

    code = ble_error(ble, [bytes([PING, 0])])
    assert code == ERR.INVALID_LENGTH, err_name(code)


def test_cancel_without_request(ble):
    """CANCEL with nothing running is ignored: no answer, link still works."""
    from ble_transport import CANCEL

    ble.write_frame(bytes([CANCEL, 0, 0]))
    with pytest.raises(TimeoutError):
        ble.read_frame(timeout=1.0)
    assert bytes(ble.call(CTAPHID.PING, b"ok")) == b"ok"


def test_msg_answer_is_framed(ble):
    """Any MSG gets a MSG (or ERROR) answer: getInfo as one byte."""
    from ble_transport import ERROR, MSG

    ble.write_frame(bytes([MSG, 0, 1, 0x04]))
    cmd, payload = ble.read_response(deadline=10)
    assert cmd in (MSG, ERROR) and payload, f"cmd {cmd:#x} {payload.hex()}"


def test_works_after_errors(ble):
    assert bytes(ble.call(CTAPHID.PING, b"still here")) == b"still here"
