"""Read-only credential management with the persistent pinUvAuthToken (pcmr).

Needs the PIN; reads the store, never changes it (the delete attempt uses a
made-up credential ID and must be refused anyway).
"""

import os

import pytest
from fido2.ctap import CtapError
from fido2.ctap2.credman import CredentialManagement
from fido2.ctap2.pin import ClientPin

from conftest import expect_error

ERR = CtapError.ERR
PCMR = ClientPin.PERMISSION.PERSISTENT_CREDENTIAL_MGMT


@pytest.fixture
def pcmr(auth, info):
    if not info.options.get("perCredMgmtRO"):
        pytest.skip("perCredMgmtRO not supported")
    cp = ClientPin(auth.ctap2)
    token = cp.get_pin_token(auth.pin, PCMR)
    return CredentialManagement(auth.ctap2, cp.protocol, token), cp, token


@pytest.mark.pin
def test_reads(pcmr, auth):
    cm, _, _ = pcmr
    meta = cm.get_metadata()
    count = meta[CredentialManagement.RESULT.EXISTING_CRED_COUNT]
    assert count >= 0
    if count:
        rps = cm.enumerate_rps()
        assert rps
        creds = cm.enumerate_creds(rps[0][CredentialManagement.RESULT.RP_ID_HASH])
        assert creds


@pytest.mark.pin
def test_cannot_delete(pcmr):
    cm, _, _ = pcmr
    descriptor = {"type": "public-key", "id": os.urandom(32)}
    expect_error(ERR.PIN_AUTH_INVALID, cm.delete_cred, descriptor)


@pytest.mark.pin
def test_the_same_token_next_time(pcmr, auth):
    _, _, token = pcmr
    again = ClientPin(auth.ctap2).get_pin_token(auth.pin, PCMR)
    assert again == token


@pytest.mark.pin
def test_pcmr_only_alone(auth, info):
    if not info.options.get("perCredMgmtRO"):
        pytest.skip("perCredMgmtRO not supported")
    expect_error(ERR.UNAUTHORIZED_PERMISSION, ClientPin(auth.ctap2).get_pin_token, auth.pin,
                 PCMR | ClientPin.PERMISSION.CREDENTIAL_MGMT)
