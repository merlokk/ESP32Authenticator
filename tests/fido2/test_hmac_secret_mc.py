"""hmac-secret-mc (CTAP 2.2): the hmac-secret output already at makeCredential.

One non-discoverable credential for ctap-test.example (one touch) and one
assertion (one touch): both outputs for the same salt must match.
"""

import hashlib

import pytest
from fido2.ctap2.pin import ClientPin, PinProtocolV2

from conftest import TEST_RP, client_data_hash

SALT = hashlib.sha256(b"ctap-test:prf-salt").digest()


def salt_input(cp):
    key_agreement, shared = cp._get_shared_secret()
    salt_enc = cp.protocol.encrypt(shared, SALT)
    salt_auth = cp.protocol.authenticate(shared, salt_enc)
    return {1: key_agreement, 2: salt_enc, 3: salt_auth, 4: cp.protocol.VERSION}, shared


@pytest.mark.touch
def test_hmac_secret_mc(auth, info):
    if "hmac-secret-mc" not in info.extensions:
        pytest.skip("hmac-secret-mc not supported")
    cp = ClientPin(auth.ctap2, PinProtocolV2())

    mc_input, mc_shared = salt_input(cp)
    att = auth.make_credential(client_data_hash("hmac-secret-mc"),
                               extensions={"hmac-secret": True, "hmac-secret-mc": mc_input})
    ext = att.auth_data.extensions
    assert ext["hmac-secret"] is True
    mc_output = cp.protocol.decrypt(mc_shared, ext["hmac-secret-mc"])
    assert len(mc_output) == 32

    ga_input, ga_shared = salt_input(cp)
    descriptor = {"type": "public-key", "id": att.auth_data.credential_data.credential_id}
    a = auth.ctap2.get_assertion(TEST_RP["id"], client_data_hash("hmac-secret-ga"), [descriptor],
                                 extensions={"hmac-secret": ga_input},
                                 on_keepalive=auth.on_keepalive)
    ga_output = cp.protocol.decrypt(ga_shared, a.auth_data.extensions["hmac-secret"])
    assert ga_output == mc_output
