"""CTAP2 authenticator test suite: fixtures and options.

Tests talk CTAP2 through python-fido2 over a pluggable transport (see
transports.py), so the same suite runs against a reference USB key and
against our BLE authenticator.
"""

import hashlib
import os
import sys

import pytest
from fido2.ctap import CtapError
from fido2.ctap2 import Ctap2
from fido2.ctap2.pin import ClientPin

import transports

TEST_RP = {"id": "ctap-test.example", "name": "CTAP test RP"}
TEST_USER = {"id": b"ctap-test-user-1", "name": "tester", "displayName": "Tester"}
ES256 = -7
STATUS_UPNEEDED = 2


def pytest_addoption(parser):
    g = parser.getgroup("fido2")
    g.addoption("--transport", default="hid", choices=transports.NAMES,
                help="authenticator transport (default: hid)")
    g.addoption("--device", default=None,
                help="pick the device whose name contains this text")
    g.addoption("--pin", default=os.environ.get("FIDO_PIN"),
                help="authenticator PIN (or FIDO_PIN env); asked if the key has one")
    g.addoption("--destructive", action="store_true",
                help="allow tests that may change authenticator state "
                     "(wrong PIN attempts)")


def pytest_configure(config):
    config.addinivalue_line("markers", "touch: needs a touch on the authenticator")
    config.addinivalue_line("markers", "pin: needs a PIN")
    config.addinivalue_line("markers", "destructive: needs --destructive")


def pytest_collection_modifyitems(config, items):
    for item in items:
        if "destructive" in item.keywords and not config.getoption("--destructive"):
            item.add_marker(pytest.mark.skip(reason="needs --destructive"))


def client_data_hash(tag: str) -> bytes:
    return hashlib.sha256(b"ctap-test:" + tag.encode()).digest()


def rp_id_hash(rp_id: str = TEST_RP["id"]) -> bytes:
    return hashlib.sha256(rp_id.encode()).digest()


def err_name(code: int) -> str:
    try:
        return CtapError.ERR(code).name
    except ValueError:
        return hex(code)


def read_secret(prompt: str) -> str:
    """Reads a line from the console, echoing '*'.

    Talks to the console directly: pytest owns stdin, and stdout may be piped
    (Tee-Object), which would hide a prompt without a newline.
    """
    sys.stdout.flush()
    if sys.platform == "win32":
        import msvcrt

        def put(text):
            for ch in text:
                msvcrt.putwch(ch)

        while msvcrt.kbhit():  # drop keys typed before the prompt
            msvcrt.getwch()
        put(prompt)
        chars = []
        while (c := msvcrt.getwch()) not in "\r\n":
            if c == "\x03":
                raise KeyboardInterrupt
            if c == "\b":
                if chars:
                    chars.pop()
                    put("\b \b")
            else:
                chars.append(c)
                put("*")
        put("\r\n")
        return "".join(chars)
    import termios
    with open("/dev/tty", "r+") as tty:
        tty.write(prompt)
        tty.flush()
        old = termios.tcgetattr(tty)
        new = termios.tcgetattr(tty)
        new[3] &= ~termios.ECHO
        try:
            termios.tcsetattr(tty, termios.TCSADRAIN, new)
            line = tty.readline().rstrip("\n")
        finally:
            termios.tcsetattr(tty, termios.TCSADRAIN, old)
        tty.write("\n")
        return line


PIN_FATAL = (CtapError.ERR.PIN_INVALID, CtapError.ERR.PIN_AUTH_BLOCKED,
             CtapError.ERR.PIN_BLOCKED)
MIN_PIN_RETRIES = 4  # do not ask for the PIN below this


def check_pin(ctap2, pin):
    """Asks for the PIN if not given and verifies it once.

    Exits the session on a wrong PIN: retrying would burn PIN retries
    (3 wrong in a row block until replug, 8 in total lock the PIN).
    """
    cp = ClientPin(ctap2)
    retries, _ = cp.get_pin_retries()
    if retries < MIN_PIN_RETRIES:
        pytest.exit(f"only {retries} PIN retries left: enter the PIN in a trusted "
                    "client (e.g. Yubico Authenticator) first", returncode=3)
    if not pin:
        print("\n" + "=" * 60)
        print(f"  The authenticator has a PIN. {retries} attempts left.")
        print("  Type the PIN and press Enter (empty: skip PIN tests).")
        print("=" * 60)
        pin = read_secret("PIN: ")
        if not pin:
            return None
    try:
        cp.get_pin_token(pin)
    except CtapError as e:
        if e.code == CtapError.ERR.PIN_INVALID:
            left, _ = cp.get_pin_retries()
            pytest.exit(f"wrong PIN, {left} attempts left; not retrying", returncode=3)
        if e.code == CtapError.ERR.PIN_AUTH_BLOCKED:
            pytest.exit("PIN blocked after 3 wrong tries: replug the key", returncode=3)
        raise
    print("PIN ok")
    return pin


class Authenticator:
    """Ctap2 plus PIN handling and a touch prompt."""

    def __init__(self, device, pin, capman):
        self.device = device
        self.ctap2 = Ctap2(device)
        self.info = self.ctap2.info
        self.pin = pin
        self._capman = capman

    def on_keepalive(self, status):
        if status == STATUS_UPNEEDED and not getattr(self, "_prompted", False):
            self._prompted = True
            with self._capman.global_and_fixture_disabled():
                print("\n>>> touch the authenticator", flush=True)

    def _uv(self, permission, cdh, rp_id):
        """pinUvAuthParam/protocol for a request, or (None, None) without a PIN."""
        if not self.pin:
            return None, None
        cp = ClientPin(self.ctap2)
        try:
            token = cp.get_pin_token(self.pin, permission, rp_id)
        except CtapError as e:
            if e.code in PIN_FATAL:  # never retry a PIN: each try burns a retry
                pytest.exit(f"PIN rejected: {err_name(e.code)}", returncode=3)
            raise
        return cp.protocol.authenticate(token, cdh), cp.protocol.VERSION

    def pin_needed_for_make_credential(self, rk=False):
        o = self.info.options
        return bool(o.get("clientPin")) and (
            rk or o.get("alwaysUv") or not o.get("makeCredUvNotRqd"))

    def make_credential(self, cdh, rp=TEST_RP, user=TEST_USER, key_params=None,
                        exclude_list=None, rk=False, extensions=None):
        self._prompted = False
        param, proto = (None, None)
        if self.pin_needed_for_make_credential(rk) or (rk and self.pin):
            param, proto = self._uv(ClientPin.PERMISSION.MAKE_CREDENTIAL, cdh, rp["id"])
        return self.ctap2.make_credential(
            cdh, rp, user, key_params or [{"type": "public-key", "alg": ES256}],
            exclude_list=exclude_list, extensions=extensions,
            options={"rk": True} if rk else None,
            pin_uv_param=param, pin_uv_protocol=proto,
            on_keepalive=self.on_keepalive)

    def get_assertion(self, cdh, rp_id=TEST_RP["id"], allow_list=None, up=True,
                      uv=False):
        self._prompted = False
        param, proto = (None, None)
        if uv:
            param, proto = self._uv(ClientPin.PERMISSION.GET_ASSERTION, cdh, rp_id)
        return self.ctap2.get_assertion(
            rp_id, cdh, allow_list, options=None if up else {"up": False},
            pin_uv_param=param, pin_uv_protocol=proto,
            on_keepalive=self.on_keepalive)


@pytest.fixture(scope="session")
def device(request):
    dev = transports.open_device(request.config.getoption("--transport"),
                                 request.config.getoption("--device"))
    yield dev
    dev.close()


@pytest.fixture(scope="session")
def auth(request, device):
    capman = request.config.pluginmanager.getplugin("capturemanager")
    pin = request.config.getoption("--pin")
    info = Ctap2(device).info
    with capman.global_and_fixture_disabled():
        print(f"\nauthenticator: {info.versions}, firmware {info.firmware_version:#x}, "
              f"aaguid {info.aaguid}\noptions: {info.options}")
        if info.options.get("clientPin"):
            pin = check_pin(Ctap2(device), pin)
    return Authenticator(device, pin, capman)


@pytest.fixture(autouse=True)
def _needs_pin(request):
    if request.node.get_closest_marker("pin") and not request.getfixturevalue("auth").pin:
        pytest.skip("needs a PIN")


@pytest.fixture(scope="session")
def info(auth):
    return auth.info


@pytest.fixture(scope="session")
def credential(auth):
    """One non-discoverable ES256 credential shared by the tests (one touch)."""
    if auth.pin_needed_for_make_credential() and not auth.pin:
        pytest.skip("the authenticator requires a PIN for makeCredential")
    cdh = client_data_hash("shared-credential")
    att = auth.make_credential(cdh)
    return cdh, att


@pytest.fixture(scope="session")
def cred_descriptor(credential):
    _, att = credential
    return {"type": "public-key", "id": att.auth_data.credential_data.credential_id}


@pytest.fixture(scope="session")
def cred_public_key(credential):
    return credential[1].auth_data.credential_data.public_key


def expect_error(code, fn, *args, **kwargs):
    """Runs fn and checks that it fails with CtapError `code` (or one of codes)."""
    codes = code if isinstance(code, (list, tuple, set)) else [code]
    with pytest.raises(CtapError) as e:
        fn(*args, **kwargs)
    got = e.value.code
    assert got in codes, f"got {err_name(got)}, want {[err_name(c) for c in codes]}"
