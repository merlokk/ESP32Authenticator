"""Built-in UV not configured yet ("uv": false) and getInfo versions.

No touch, no PIN.
"""

import pytest
from fido2.ctap import CtapError
from fido2.ctap2.pin import ClientPin

from conftest import expect_error

ERR = CtapError.ERR


def test_versions(info):
    assert "FIDO_2_2" not in info.versions  # not a version string
    if "FIDO_2_3" in info.versions:
        assert "FIDO_2_1" in info.versions


def test_uv_not_configured(auth, info):
    if info.options.get("uv") is not False:
        pytest.skip("no built-in UV that is not configured")
    cp = ClientPin(auth.ctap2)
    assert cp.get_uv_retries() > 0
    expect_error(ERR.NOT_ALLOWED, cp.get_uv_token, ClientPin.PERMISSION.CREDENTIAL_MGMT)


def test_uv_option_on_make_credential_is_refused(auth, info):
    if info.options.get("uv") is not False:
        pytest.skip("no built-in UV that is not configured")
    expect_error(ERR.INVALID_OPTION, auth.ctap2.make_credential,
                 b"\x00" * 32, {"id": "ctap-test.example"}, {"id": b"u"},
                 [{"type": "public-key", "alg": -7}], options={"uv": True})
