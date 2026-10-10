"""authenticatorClientPIN (0x06). Tests never set or change the PIN."""

import pytest
from fido2.ctap import CtapError
from fido2.ctap2.pin import ClientPin

import transports
from conftest import client_data_hash, err_name, expect_error

ERR = CtapError.ERR


@pytest.fixture
def client_pin(auth):
    if "clientPin" not in auth.info.options:
        pytest.skip("clientPin not supported")
    return ClientPin(auth.ctap2)


def test_retries(client_pin, info):
    if not info.options["clientPin"]:
        # PIN not set: YubiKey 5.8 answers PIN_NOT_SET, others may give the count
        try:
            retries, _ = client_pin.get_pin_retries()
        except CtapError as e:
            assert e.code == ERR.PIN_NOT_SET, err_name(e.code)
            return
    else:
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


@pytest.mark.pin
@pytest.mark.destructive
@pytest.mark.replug
def test_pin_auth_blocked(request, auth):
    """3 wrong PINs in a row block PIN auth until a power cycle, even for the
    right PIN, without burning more retries; after a replug the right PIN
    works and restores the retries."""
    cp = ClientPin(auth.ctap2)
    before, _ = cp.get_pin_retries()
    if before < 6:
        pytest.skip(f"only {before} PIN retries left")
    wrong = auth.pin + "x"
    expect_error(ERR.PIN_INVALID, cp.get_pin_token, wrong)
    expect_error(ERR.PIN_INVALID, cp.get_pin_token, wrong)
    # CTAP 2.1: the 3rd mismatch in a row already answers PIN_AUTH_BLOCKED;
    # 2.0 authenticators may answer PIN_INVALID and block from the next try
    expect_error([ERR.PIN_AUTH_BLOCKED, ERR.PIN_INVALID], cp.get_pin_token, wrong)
    assert cp.get_pin_retries()[0] == before - 3
    expect_error(ERR.PIN_AUTH_BLOCKED, cp.get_pin_token, auth.pin)
    assert cp.get_pin_retries()[0] == before - 3

    auth.say("\n  PIN auth blocked: power cycle")
    auth.attach(transports.wait_replug(auth.device, request.config, auth.say))
    cp = ClientPin(auth.ctap2)
    assert cp.get_pin_retries()[0] == before - 3
    cp.get_pin_token(auth.pin)
    assert cp.get_pin_retries()[0] == before
