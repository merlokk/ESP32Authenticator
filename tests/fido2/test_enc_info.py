"""getInfo encIdentifier (0x19) / encCredStoreState (0x1E), CTAP 2.2.

Needs the PIN (a pcmr token); reads only.
"""

import pytest
from fido2.ctap2.pin import ClientPin, PinProtocolV1, PinProtocolV2

PCMR = ClientPin.PERMISSION.PERSISTENT_CREDENTIAL_MGMT


@pytest.fixture
def persistent_token(auth, info):
    if not info.options.get("perCredMgmtRO"):
        pytest.skip("perCredMgmtRO not supported")
    return ClientPin(auth.ctap2, PinProtocolV2()).get_pin_token(auth.pin, PCMR)


@pytest.mark.pin
def test_identifier(auth, persistent_token):
    info = auth.ctap2.get_info()
    if not info.enc_identifier:
        pytest.skip("no encIdentifier")
    identifier = info.get_identifier(persistent_token)
    assert len(identifier) == 16
    again = auth.ctap2.get_info()
    assert again.enc_identifier != info.enc_identifier  # a fresh iv
    assert again.get_identifier(persistent_token) == identifier


@pytest.mark.pin
def test_cred_store_state_is_stable(auth, persistent_token):
    info = auth.ctap2.get_info()
    if not info.enc_cred_store_state:
        pytest.skip("no encCredStoreState")
    state = info.get_cred_store_state(persistent_token)
    assert len(state) == 16
    assert auth.ctap2.get_info().get_cred_store_state(persistent_token) == state


@pytest.mark.pin
def test_both_protocols_get_the_same_token(auth, persistent_token):
    v1 = ClientPin(auth.ctap2, PinProtocolV1()).get_pin_token(auth.pin, PCMR)
    assert v1 == persistent_token
