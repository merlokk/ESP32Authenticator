"""The credential store fills up to the authenticator's limit (--fill).

Makes discoverable credentials for one test RP until the authenticator
answers KEY_STORE_FULL, checks that the count matches what getCredsMetadata
promised, then deletes them all. One touch per credential: on our firmware
turn on `config test presence on` first (and off after).
"""

import faulthandler

import pytest
from fido2.ctap import CtapError
from fido2.ctap2 import CredentialManagement
from fido2.ctap2.pin import ClientPin

from conftest import client_data_hash

ERR = CtapError.ERR
pytestmark = [pytest.mark.pin, pytest.mark.fill]

FILL_RP = {"id": "ctap-test-fill.example", "name": "CTAP test fill"}
MAX_TRIES = 300  # far above any limit: a store that never fills fails the test
GUARD_S = 180    # per credential (pytest.ini's guard is per test)


def credman(auth):
    cp = ClientPin(auth.ctap2)
    token = cp.get_pin_token(auth.pin, ClientPin.PERMISSION.CREDENTIAL_MGMT)
    return CredentialManagement(auth.ctap2, cp.protocol, token)


def delete_fill_creds(auth):
    cm = credman(auth)
    deleted = 0
    for rp in cm.enumerate_rps():
        if rp[CredentialManagement.RESULT.RP]["id"] == FILL_RP["id"]:
            for c in cm.enumerate_creds(rp[CredentialManagement.RESULT.RP_ID_HASH]):
                cm.delete_cred(c[CredentialManagement.RESULT.CREDENTIAL_ID])
                deleted += 1
                faulthandler.dump_traceback_later(GUARD_S, exit=True)
    return deleted


def test_store_fills_to_the_limit(auth):
    if not auth.info.options.get("rk") or not CredentialManagement.is_supported(auth.info):
        pytest.skip("needs discoverable credentials and credentialManagement")
    delete_fill_creds(auth)  # leftovers of a cut run
    meta = credman(auth).get_metadata()
    remaining = meta[CredentialManagement.RESULT.MAX_REMAINING_COUNT]
    auth.say(f"\n>>> filling: {meta[CredentialManagement.RESULT.EXISTING_CRED_COUNT]} "
             f"discoverable stored, room for {remaining}")
    made = 0
    full = False
    try:
        for i in range(MAX_TRIES):
            faulthandler.dump_traceback_later(GUARD_S, exit=True)
            user = {"id": f"fill-{i:03d}".encode(), "name": f"fill-{i:03d}",
                    "displayName": f"Fill {i}"}
            try:
                auth.make_credential(client_data_hash(f"fill-{i}"), rp=FILL_RP, user=user,
                                     rk=True)
            except CtapError as e:
                assert e.code == ERR.KEY_STORE_FULL, f"credential {i + 1}: {e}"
                full = True
                break
            made += 1
            if made % 25 == 0:
                auth.say(f">>> {made} made")
        assert full, f"no KEY_STORE_FULL after {MAX_TRIES} credentials"
        assert made == remaining, f"made {made}, getCredsMetadata promised {remaining}"
        after = credman(auth).get_metadata()
        assert after[CredentialManagement.RESULT.MAX_REMAINING_COUNT] == 0
        auth.say(f">>> full after {made}: KEY_STORE_FULL")
    finally:
        deleted = delete_fill_creds(auth)
        auth.say(f">>> deleted {deleted} fill credential(s)")
    assert deleted == made
