"""authenticatorConfig (0x0D).

Never calls setMinPINLength on the device: only a FIDO reset lowers the
minimum again (it is covered by LionKey's own tests). toggleAlwaysUv is
switched on and back off only with --destructive.
"""

import pytest
from fido2 import cbor
from fido2.ctap import CtapError
from fido2.ctap2 import Ctap2
from fido2.ctap2.config import Config
from fido2.ctap2.pin import ClientPin

from conftest import expect_error

ERR = CtapError.ERR
CMD_CONFIG = 0x0D
TOGGLE_ALWAYS_UV = 0x02
ENABLE_ENTERPRISE_ATTESTATION = 0x01


@pytest.fixture
def raw_info(device):
    """getInfo as a map with integer keys (members python-fido2 may not know)."""
    resp = device.call(0x10, bytes([Ctap2.CMD.GET_INFO]))  # CTAPHID.CBOR
    assert resp[0] == 0
    return cbor.decode(resp[1:])


@pytest.fixture
def config_supported(info):
    if not info.options.get("authnrCfg"):
        pytest.skip("authenticatorConfig not supported")


def config_token(auth):
    cp = ClientPin(auth.ctap2)
    token = cp.get_pin_token(auth.pin, ClientPin.PERMISSION.AUTHENTICATOR_CFG)
    return cp.protocol, token


def test_advertised(config_supported, info, raw_info):
    assert info.options.get("setMinPINLength") is True
    assert "alwaysUv" in info.options
    if info.options["alwaysUv"]:
        assert not info.options.get("makeCredUvNotRqd")
    # maxRPIDsForSetMinPINLength: present iff setMinPINLength is supported
    assert 0x10 in raw_info
    assert isinstance(raw_info.get(0x0C, False), bool)  # forcePINChange


def test_config_commands(config_supported, raw_info):
    """authenticatorConfigCommands (0x1F, CTAP 2.3), when present."""
    if 0x1F not in raw_info:
        pytest.skip("no authenticatorConfigCommands (pre-2.3)")
    commands = raw_info[0x1F]
    assert TOGGLE_ALWAYS_UV in commands
    assert 0x03 in commands  # setMinPINLength


def test_long_touch_for_reset(raw_info):
    """longTouchForReset (0x18, CTAP 2.3), when present, is a bool."""
    if 0x18 not in raw_info:
        pytest.skip("no longTouchForReset (pre-2.3)")
    assert isinstance(raw_info[0x18], bool)


def test_unsupported_subcommand(config_supported, auth, info):
    if info.options.get("ep") is not None:
        pytest.skip("enterprise attestation supported")
    expect_error(ERR.INVALID_PARAMETER, auth.ctap2.send_cbor, CMD_CONFIG,
                 {1: ENABLE_ENTERPRISE_ATTESTATION})


def test_missing_subcommand(config_supported, auth):
    expect_error(ERR.MISSING_PARAMETER, auth.ctap2.send_cbor, CMD_CONFIG, {3: 2})


@pytest.mark.pin
def test_needs_pin_uv_auth_param(config_supported, auth, info):
    if not info.options.get("clientPin"):
        pytest.skip("no PIN set: authenticatorConfig runs without one")
    expect_error(ERR.PUAT_REQUIRED, auth.ctap2.send_cbor, CMD_CONFIG,
                 {1: TOGGLE_ALWAYS_UV})


@pytest.mark.pin
def test_wrong_pin_uv_auth_param(config_supported, auth):
    protocol, token = config_token(auth)
    expect_error(ERR.PIN_AUTH_INVALID, auth.ctap2.send_cbor, CMD_CONFIG,
                 {1: TOGGLE_ALWAYS_UV, 3: protocol.VERSION, 4: b"\x00" * 32})


@pytest.mark.pin
def test_token_without_acfg(config_supported, auth):
    cp = ClientPin(auth.ctap2)
    token = cp.get_pin_token(auth.pin, ClientPin.PERMISSION.CREDENTIAL_MGMT)
    config = Config(auth.ctap2, cp.protocol, token)
    expect_error(ERR.PIN_AUTH_INVALID, config.toggle_always_uv)


@pytest.mark.pin
@pytest.mark.destructive
def test_toggle_always_uv_and_back(config_supported, auth):
    before = auth.ctap2.get_info().options["alwaysUv"]
    protocol, token = config_token(auth)
    Config(auth.ctap2, protocol, token).toggle_always_uv()
    try:
        options = auth.ctap2.get_info().options
        assert options["alwaysUv"] is (not before)
        if options["alwaysUv"]:
            assert not options.get("makeCredUvNotRqd")
    finally:
        protocol, token = config_token(auth)
        Config(auth.ctap2, protocol, token).toggle_always_uv()
    assert auth.ctap2.get_info().options["alwaysUv"] is before, "alwaysUv not restored"
