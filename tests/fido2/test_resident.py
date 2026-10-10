"""Discoverable credentials and credentialManagement (0x0A).

Creates credentials only for the test RP and deletes them afterwards.
"""

import pytest
from fido2.ctap import CtapError
from fido2.ctap2 import CredentialManagement
from fido2.ctap2.pin import ClientPin

from conftest import TEST_RP, client_data_hash, expect_error, rp_id_hash

ERR = CtapError.ERR
pytestmark = pytest.mark.pin

RK_RP = {"id": "ctap-test-rk.example", "name": "CTAP test RK"}
RK_USER = {"id": b"ctap-test-rk-user", "name": "rk-tester", "displayName": "RK"}


def credman(auth):
    cp = ClientPin(auth.ctap2)
    token = cp.get_pin_token(auth.pin, ClientPin.PERMISSION.CREDENTIAL_MGMT)
    return CredentialManagement(auth.ctap2, cp.protocol, token)


def delete_test_creds(auth):
    cm = credman(auth)
    for rp in cm.enumerate_rps():
        if rp[CredentialManagement.RESULT.RP]["id"] in (RK_RP["id"], TEST_RP["id"]):
            for c in cm.enumerate_creds(rp[CredentialManagement.RESULT.RP_ID_HASH]):
                cm.delete_cred(c[CredentialManagement.RESULT.CREDENTIAL_ID])


@pytest.fixture(scope="module")
def rk(auth):
    if not auth.info.options.get("rk"):
        pytest.skip("discoverable credentials not supported")
    if not CredentialManagement.is_supported(auth.info):
        pytest.skip("credentialManagement not supported (cannot clean up)")
    delete_test_creds(auth)
    att = auth.make_credential(client_data_hash("rk"), rp=RK_RP, user=RK_USER, rk=True)
    yield att
    delete_test_creds(auth)


def test_discoverable_assertion(auth, rk):
    cdh = client_data_hash("rk-get")
    a = auth.get_assertion(cdh, rp_id=RK_RP["id"], up=False)
    a.verify(cdh, rk.auth_data.credential_data.public_key)
    assert a.user["id"] == RK_USER["id"]


def test_listed_by_credman(auth, rk):
    cm = credman(auth)
    rps = {r[CredentialManagement.RESULT.RP]["id"] for r in cm.enumerate_rps()}
    assert RK_RP["id"] in rps
    creds = cm.enumerate_creds(rp_id_hash(RK_RP["id"]))
    ids = [c[CredentialManagement.RESULT.CREDENTIAL_ID]["id"] for c in creds]
    assert rk.auth_data.credential_data.credential_id in ids


def test_metadata(auth, rk):
    meta = credman(auth).get_metadata()
    assert meta[CredentialManagement.RESULT.EXISTING_CRED_COUNT] >= 1


def test_deleted_credential_is_gone(auth, rk):
    delete_test_creds(auth)
    expect_error(ERR.NO_CREDENTIALS, auth.get_assertion, client_data_hash("rk-gone"),
                 rp_id=RK_RP["id"], up=False)
