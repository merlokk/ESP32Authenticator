"""EdDSA credentials: COSE -8 (EdDSA) and -19 (Ed25519, RFC 9864).

Each makes one non-discoverable credential for ctap-test.example (one touch);
the assertion is silent.
"""

import pytest
from fido2.attestation import Attestation

from conftest import client_data_hash

EDDSA = -8
ED25519 = -19
OKP = 1
CRV_ED25519 = 6


def supported(info, alg):
    return not info.algorithms or {"type": "public-key", "alg": alg} in info.algorithms


@pytest.mark.touch
@pytest.mark.parametrize("alg", [EDDSA, ED25519])
def test_eddsa_credential(auth, info, alg):
    if not supported(info, alg):
        pytest.skip(f"COSE {alg} not supported")
    cdh = client_data_hash(f"eddsa{alg}")
    att = auth.make_credential(cdh, key_params=[{"type": "public-key", "alg": alg}])
    key = att.auth_data.credential_data.public_key
    assert key[3] == alg
    assert key[1] == OKP
    assert key[-1] == CRV_ED25519
    assert len(key[-2]) == 32
    assert att.att_stmt["alg"] == alg
    Attestation.for_type(att.fmt)().verify(att.att_stmt, att.auth_data, cdh)

    descriptor = {"type": "public-key", "id": att.auth_data.credential_data.credential_id}
    cdh2 = client_data_hash(f"eddsa{alg}-get")
    a = auth.get_assertion(cdh2, allow_list=[descriptor], up=False)
    assert len(a.signature) == 64
    a.verify(cdh2, key)
