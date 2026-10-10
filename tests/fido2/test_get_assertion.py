"""authenticatorGetAssertion (0x02)."""

import os

import pytest
from fido2.ctap import CtapError
from fido2.ctap2 import Ctap2

from conftest import client_data_hash, expect_error, rp_id_hash

ERR = CtapError.ERR


def test_silent_assertion(auth, cred_descriptor, cred_public_key):
    cdh = client_data_hash("silent")
    a = auth.get_assertion(cdh, allow_list=[cred_descriptor], up=False)
    a.verify(cdh, cred_public_key)
    assert a.auth_data.rp_id_hash == rp_id_hash()
    assert not a.auth_data.is_user_present()
    assert a.credential["id"] == cred_descriptor["id"]


@pytest.mark.touch
def test_assertion_with_touch(auth, cred_descriptor, cred_public_key):
    cdh = client_data_hash("touch")
    a = auth.get_assertion(cdh, allow_list=[cred_descriptor])
    a.verify(cdh, cred_public_key)
    assert a.auth_data.is_user_present()


def test_counter_increases(auth, credential, cred_descriptor):
    def counter():
        return auth.get_assertion(client_data_hash("cnt"), allow_list=[cred_descriptor],
                                  up=False).auth_data.counter

    c1, c2 = counter(), counter()
    assert c2 > c1 or c1 == c2 == 0  # 0 = counter not supported
    if c1:
        assert c1 > credential[1].auth_data.counter


def test_signature_bound_to_client_data(auth, cred_descriptor, cred_public_key):
    a = auth.get_assertion(client_data_hash("a"), allow_list=[cred_descriptor], up=False)
    with pytest.raises(Exception):
        a.verify(client_data_hash("b"), cred_public_key)


def test_unknown_credential(auth):
    bogus = {"type": "public-key", "id": os.urandom(64)}
    expect_error(ERR.NO_CREDENTIALS, auth.get_assertion, client_data_hash("unknown"),
                 allow_list=[bogus], up=False)


def test_wrong_rp(auth, cred_descriptor):
    expect_error(ERR.NO_CREDENTIALS, auth.get_assertion, client_data_hash("rp"),
                 rp_id="other-rp.example", allow_list=[cred_descriptor], up=False)


def test_no_discoverable_credentials(auth):
    expect_error(ERR.NO_CREDENTIALS, auth.get_assertion, client_data_hash("none"),
                 rp_id="no-creds.example", up=False)


@pytest.mark.parametrize("drop", [1, 2])
def test_missing_parameter(auth, drop):
    req = {1: "ctap-test.example", 2: client_data_hash("missing")}
    del req[drop]
    expect_error(ERR.MISSING_PARAMETER, auth.ctap2.send_cbor,
                 Ctap2.CMD.GET_ASSERTION, req)


def test_get_next_assertion_without_context(auth):
    expect_error(ERR.NOT_ALLOWED, auth.ctap2.send_cbor, Ctap2.CMD.GET_NEXT_ASSERTION)
