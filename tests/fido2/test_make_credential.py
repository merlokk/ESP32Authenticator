"""authenticatorMakeCredential (0x01)."""

import pytest
from fido2.attestation import Attestation
from fido2.ctap import CtapError
from fido2.ctap2 import Ctap2

from conftest import ES256, TEST_RP, TEST_USER, client_data_hash, expect_error, rp_id_hash

ERR = CtapError.ERR
PUB_KEY_ES256 = [{"type": "public-key", "alg": ES256}]


def test_attestation_verifies(credential):
    cdh, att = credential
    Attestation.for_type(att.fmt)().verify(att.att_stmt, att.auth_data, cdh)


def test_auth_data(credential, info):
    _, att = credential
    ad = att.auth_data
    assert ad.rp_id_hash == rp_id_hash()
    assert ad.is_user_present()
    assert ad.is_attested()
    assert bytes(ad.credential_data.aaguid) == bytes(info.aaguid)


def test_credential_id_length(credential, info):
    cred_id = credential[1].auth_data.credential_data.credential_id
    assert 16 <= len(cred_id) <= 1023
    if info.max_cred_id_length:
        assert len(cred_id) <= info.max_cred_id_length


def test_public_key_es256(cred_public_key):
    assert cred_public_key[3] == ES256  # alg
    assert cred_public_key[1] == 2  # kty EC2
    assert cred_public_key[-1] == 1  # crv P-256


def test_unsupported_algorithm(auth):
    expect_error(ERR.UNSUPPORTED_ALGORITHM, auth.make_credential,
                 client_data_hash("bad-alg"),
                 key_params=[{"type": "public-key", "alg": -65535}])


@pytest.mark.parametrize("drop", [1, 2, 3, 4])
def test_missing_parameter(auth, drop):
    """Each of clientDataHash/rp/user/pubKeyCredParams is required."""
    req = {1: client_data_hash("missing"), 2: TEST_RP, 3: TEST_USER,
           4: PUB_KEY_ES256}
    del req[drop]
    # YubiKey answers UNSUPPORTED_ALGORITHM for a missing pubKeyCredParams
    codes = [ERR.MISSING_PARAMETER] + ([ERR.UNSUPPORTED_ALGORITHM] if drop == 4 else [])
    expect_error(codes, auth.ctap2.send_cbor,
                 Ctap2.CMD.MAKE_CREDENTIAL, req)


def test_wrong_type(auth):
    req = {1: client_data_hash("type"), 2: 42, 3: TEST_USER, 4: PUB_KEY_ES256}
    expect_error([ERR.CBOR_UNEXPECTED_TYPE, ERR.INVALID_CBOR],
                 auth.ctap2.send_cbor, Ctap2.CMD.MAKE_CREDENTIAL, req)


def test_up_false_is_invalid(auth):
    """CTAP 2.1: makeCredential with options.up=false is an invalid option."""
    req = {1: client_data_hash("up"), 2: TEST_RP, 3: TEST_USER,
           4: PUB_KEY_ES256, 7: {"up": False}}
    expect_error(ERR.INVALID_OPTION, auth.ctap2.send_cbor,
                 Ctap2.CMD.MAKE_CREDENTIAL, req)


@pytest.mark.touch
def test_exclude_list(auth, cred_descriptor):
    expect_error(ERR.CREDENTIAL_EXCLUDED, auth.make_credential,
                 client_data_hash("exclude"), exclude_list=[cred_descriptor])
