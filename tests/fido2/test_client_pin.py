"""authenticatorClientPIN (0x06). Tests never set or change the PIN."""

import pytest
from fido2.ctap import CtapError
from fido2.ctap2.pin import ClientPin

from conftest import client_data_hash, expect_error

ERR = CtapError.ERR


@pytest.fixture
def client_pin(auth):
    if "clientPin" not in auth.info.options:
        pytest.skip("clientPin not supported")
    return ClientPin(auth.ctap2)


def test_retries(client_pin):
    retries, _ = client_pin.get_pin_retries()
    assert 0 <= retries <= 8


def test_key_agreement(client_pin):
    r = client_pin.ctap.client_pin(client_pin.protocol.VERSION,
                                   ClientPin.CMD.GET_KEY_AGREEMENT)
    key = r[ClientPin.RESULT.KEY_AGREEMENT]
    assert key[1] == 2 and key[-1] == 1  # kty EC2, crv P-256
    assert len(key[-2]) == 32 and len(key[-3]) == 32


@pytest.mark.pin
def test_pin_token(client_pin, auth):
    token = client_pin.get_pin_token(auth.pin, ClientPin.PERMISSION.GET_ASSERTION,
                                     "ctap-test.example")
    assert len(token) in (16, 32)


@pytest.mark.pin
def test_uv_assertion(auth, cred_descriptor, cred_public_key):
    cdh = client_data_hash("uv")
    a = auth.get_assertion(cdh, allow_list=[cred_descriptor], up=False, uv=True)
    a.verify(cdh, cred_public_key)
    assert a.auth_data.is_user_verified()


@pytest.mark.pin
def test_bad_pin_auth_param(auth, client_pin, cred_descriptor):
    expect_error(ERR.PIN_AUTH_INVALID, auth.ctap2.get_assertion, "ctap-test.example",
                 client_data_hash("bad"), [cred_descriptor], options={"up": False},
                 pin_uv_param=b"\0" * 32, pin_uv_protocol=client_pin.protocol.VERSION)


@pytest.mark.pin
@pytest.mark.destructive
def test_wrong_pin(auth, client_pin):
    """One wrong attempt decrements retries; the right PIN restores them."""
    before, _ = client_pin.get_pin_retries()
    if before < 4:
        pytest.skip(f"only {before} PIN retries left")
    expect_error(ERR.PIN_INVALID, client_pin.get_pin_token, auth.pin + "x")
    assert client_pin.get_pin_retries()[0] == before - 1
    client_pin.get_pin_token(auth.pin, ClientPin.PERMISSION.GET_ASSERTION,
                             "ctap-test.example")
    assert client_pin.get_pin_retries()[0] == before
