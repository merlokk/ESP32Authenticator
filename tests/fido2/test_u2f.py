"""U2F / CTAP1 raw messages (U2F_VERSION, U2F_REGISTER, U2F_AUTHENTICATE).

One registration for https://ctap-test.example (one touch) and one
authentication with user presence (one touch); the rest is silent.
"""

import hashlib

import pytest
from fido2.ctap1 import APDU, ApduError, Ctap1

APP = hashlib.sha256(b"https://ctap-test.example").digest()
CHALLENGE = hashlib.sha256(b"ctap-test:u2f").digest()


@pytest.fixture(scope="module")
def ctap1(auth):
    return Ctap1(auth.device)


@pytest.fixture(scope="module")
def registration(ctap1):
    reg = ctap1.register(CHALLENGE, APP)
    return reg


def test_version(ctap1):
    assert ctap1.get_version() == "U2F_V2"


@pytest.mark.touch
def test_register(registration):
    # the attestation: the self-signed certificate's key signs the registration
    registration.verify(APP, CHALLENGE)
    assert len(registration.key_handle) == 32
    assert len(registration.public_key) == 65


@pytest.mark.touch
def test_check_only(ctap1, registration):
    with pytest.raises(ApduError) as e:
        ctap1.authenticate(CHALLENGE, APP, registration.key_handle, check_only=True)
    assert e.value.code == APDU.USE_NOT_SATISFIED  # the key handle is ours


@pytest.mark.touch
def test_wrong_application(ctap1, registration):
    other = hashlib.sha256(b"https://other.example").digest()
    with pytest.raises(ApduError) as e:
        ctap1.authenticate(CHALLENGE, other, registration.key_handle, check_only=True)
    assert e.value.code == APDU.WRONG_DATA


@pytest.mark.touch
def test_authenticate(ctap1, registration):
    sig = ctap1.authenticate(CHALLENGE, APP, registration.key_handle)
    assert sig.user_presence & 1
    sig.verify(APP, CHALLENGE, registration.public_key)
