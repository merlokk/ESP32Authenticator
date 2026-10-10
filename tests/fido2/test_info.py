"""authenticatorGetInfo (0x04)."""

from fido2 import cbor
from fido2.ctap2 import Ctap2

from conftest import ES256


def test_versions(info):
    assert {"FIDO_2_0", "FIDO_2_1", "FIDO_2_1_PRE"} & set(info.versions)


def test_aaguid(info):
    assert len(bytes(info.aaguid)) == 16


def test_options_are_bool(info):
    assert all(isinstance(v, bool) for v in info.options.values())


def test_pin_protocols(info):
    if "clientPin" in info.options:
        assert set(info.pin_uv_protocols) & {1, 2}


def test_es256_supported(info):
    if info.algorithms:  # absent means ES256 only
        assert {"type": "public-key", "alg": ES256} in info.algorithms


def test_max_msg_size(info):
    assert info.max_msg_size >= 1024


def test_canonical_cbor(device):
    """The response is CTAP2 canonical CBOR: it re-encodes to the same bytes."""
    resp = device.call(0x10, bytes([Ctap2.CMD.GET_INFO]))  # CTAPHID.CBOR
    assert resp[0] == 0
    assert cbor.encode(cbor.decode(resp[1:])) == resp[1:]
