"""Attestation formats (CTAP 2.2): getInfo attestationFormats (0x16) and
makeCredential attestationFormatsPreference (0x0B).

One non-discoverable credential for ctap-test.example with "none" (one touch).
"""

import pytest
from fido2.ctap2 import Ctap2
from fido2.webauthn import AuthenticatorData

from conftest import ES256, TEST_RP, TEST_USER, client_data_hash


def test_get_info(info):
    if not info.attestation_formats:
        pytest.skip("no attestationFormats (pre-2.2)")
    assert "none" not in info.attestation_formats  # implied, MUST NOT be listed
    assert len(set(info.attestation_formats)) == len(info.attestation_formats)


@pytest.mark.touch
def test_none(auth, info):
    if not info.attestation_formats:
        pytest.skip("no attestationFormats (pre-2.2)")
    if auth.pin_needed_for_make_credential():
        pytest.skip("the authenticator requires a PIN for makeCredential")
    auth._prompted = False
    resp = auth.ctap2.send_cbor(
        Ctap2.CMD.MAKE_CREDENTIAL,
        {
            1: client_data_hash("attestation-none"),
            2: TEST_RP,
            3: TEST_USER,
            4: [{"type": "public-key", "alg": ES256}],
            11: ["none"],
        },
        on_keepalive=auth.on_keepalive,
    )
    assert resp[1] == "none"
    assert resp[3] == {}
    auth_data = AuthenticatorData(resp[2])
    assert auth_data.is_attested()
    assert auth_data.credential_data.public_key[3] == ES256
