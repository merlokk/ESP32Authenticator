"""getInfo of our firmware (the LionKey fork): every feature it must offer.

The feature tests skip when getInfo lacks a feature (the suite also runs
against other keys); here a feature that disappears fails. Only for the
LionKey AAGUID. No touch, no PIN.
"""

import pytest
from fido2 import cbor
from fido2.ctap2 import Ctap2

LIONKEY_AAGUID = bytes.fromhex("a8a147326b7d120dfb917356bc189803")
REVISION_U2F_1_2 = 0x40
REVISION_FIDO2 = 0x20
UV_MODALITY_PASSCODE_INTERNAL = 0x04
TRANSPORTS_FOR_RESET = 0x1A


@pytest.fixture
def ours(info):
    if bytes(info.aaguid) != LIONKEY_AAGUID:
        pytest.skip("not the LionKey firmware")
    return info


def test_versions(ours):
    assert set(ours.versions) == {"U2F_V2", "FIDO_2_0", "FIDO_2_1", "FIDO_2_3"}


def test_algorithms(ours):
    assert [a["alg"] for a in ours.algorithms] == [-7, -8, -19]


def test_extensions(ours):
    want = {"credProtect", "hmac-secret", "minPinLength", "hmac-secret-mc"}
    assert want <= set(ours.extensions)


def test_options(ours):
    o = ours.options
    for name in ("rk", "up", "clientPin", "pinUvAuthToken", "credMgmt",
                 "authnrCfg", "setMinPINLength", "perCredMgmtRO"):
        assert name in o, name
    assert o["perCredMgmtRO"] is True
    assert o["authnrCfg"] is True
    assert o["uv"] is False  # built-in UV not configured yet


def test_uv_modality(ours):
    assert ours.uv_modality == UV_MODALITY_PASSCODE_INTERNAL


def test_attestation_formats(ours):
    assert ours.attestation_formats == ["packed"]  # "none" is implied


def test_config(ours, device):
    assert ours.authenticator_config_commands == [2, 3, 4]
    assert ours.max_rpids_for_min_pin == 8
    assert ours.long_touch_for_reset is True
    # absent = every transport (python-fido2 shows an absent one as [])
    resp = device.call(0x10, bytes([Ctap2.CMD.GET_INFO]))  # CTAPHID.CBOR
    assert resp[0] == 0
    assert TRANSPORTS_FOR_RESET not in cbor.decode(resp[1:])
    assert ours.pin_complexity_policy is not None
    # listed as an extension exactly while on
    assert ours.pin_complexity_policy == ("pinComplexityPolicy" in ours.extensions)


def test_pin_protocols(ours):
    assert set(ours.pin_uv_protocols) == {1, 2}


def test_ble_revision(ours, device):
    from ble_transport import BleCtapDevice

    if not isinstance(device, BleCtapDevice):
        pytest.skip("BLE only")
    # Always both (what we support), also after a client picked one: Windows
    # keeps the link of a bonded host up, so an earlier pick may be in place.
    assert device.revision == REVISION_U2F_1_2 | REVISION_FIDO2
