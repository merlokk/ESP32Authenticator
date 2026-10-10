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
                     "(wrong PIN attempts) or wedge it until replug")
    g.addoption("--set-pin", action="store_true",
                help="set a PIN on an authenticator that has none (asked twice); "
                     "only a FIDO reset removes it")
    g.addoption("--reset", action="store_true",
                help="run the authenticatorReset tests (last): erase ALL FIDO "
                     "credentials and the PIN; asks to type RESET")


def pytest_configure(config):
    config.addinivalue_line("markers", "touch: needs a touch on the authenticator")
    config.addinivalue_line("markers", "pin: needs a PIN")
    config.addinivalue_line("markers", "destructive: needs --destructive")
    config.addinivalue_line("markers", "reset: needs --reset, runs last")
    config.addinivalue_line("markers", "replug: asks to replug the authenticator, runs late")
    config.addinivalue_line("markers", "late: runs after the other tests (before reset)")
    config.addinivalue_line("markers", "transport: transport level, runs without CTAP2")


def pytest_sessionstart(session):
    """Opens the authenticator and settles the PIN before any test runs."""
    config = session.config
    if config.option.collectonly:
        return
    capman = config.pluginmanager.getplugin("capturemanager")
    device = transports.open_device(config.getoption("--transport"),
                                    config.getoption("--device"))
    pin = config.getoption("--pin")
    pin = Secret(pin) if pin else None
    try:
        info = Ctap2(device).info
    except CtapError as e:
        if e.code == CtapError.ERR.CHANNEL_BUSY:
            pytest.exit("the authenticator stays busy: replug it", returncode=2)
        # No working CTAP2 (e.g. our firmware without a CTAP core yet): only
        # the transport tests run.
        with capman.global_and_fixture_disabled():
            print(f"\nauthenticator: {device}\ngetInfo failed: {e}; "
                  "running transport tests only")
        config.fido_auth = Authenticator(device, None, capman)
        return
    with capman.global_and_fixture_disabled():
        if config.getoption("--reset"):
            confirm_reset()
        print(f"\nauthenticator: {info.versions}, firmware {info.firmware_version:#x}, "
              f"aaguid {info.aaguid}\noptions: {info.options}")
        if config.getoption("--set-pin"):
            if "clientPin" not in info.options:
                pytest.exit("--set-pin: the authenticator has no PIN support", returncode=2)
            if info.options["clientPin"]:
                pytest.exit("--set-pin: the authenticator already has a PIN; "
                            "it is never changed", returncode=2)
            pin = set_pin(Ctap2(device), info.min_pin_length)
        elif info.options.get("clientPin"):
            pin = check_pin(Ctap2(device), pin)
        else:
            pin = None  # no PIN set on the authenticator
    config.fido_auth = Authenticator(device, pin, capman)


def pytest_sessionfinish(session):
    auth = getattr(session.config, "fido_auth", None)
    if auth:
        auth.device.close()


def pytest_collection_modifyitems(config, items):
    auth = getattr(config, "fido_auth", None)
    # stable sort: late and replug tests, then reset tests, run last
    items.sort(key=lambda item: 2 * ("reset" in item.keywords)
               + ("replug" in item.keywords or "late" in item.keywords))
    for item in items:
        if "reset" in item.keywords and not config.getoption("--reset"):
            item.add_marker(pytest.mark.skip(reason="needs --reset"))
        if "destructive" in item.keywords and not config.getoption("--destructive"):
            item.add_marker(pytest.mark.skip(reason="needs --destructive"))
        if "pin" in item.keywords and auth and not auth.pin:
            item.add_marker(pytest.mark.skip(reason="needs a PIN"))
        if "replug" in item.keywords and config.getoption("--transport") != "hid":
            item.add_marker(pytest.mark.skip(reason="replug: hid only"))
        if auth and auth.info is None and "transport" not in item.keywords:
            item.add_marker(pytest.mark.skip(reason="no CTAP2 (getInfo failed)"))


def client_data_hash(tag: str) -> bytes:
    return hashlib.sha256(b"ctap-test:" + tag.encode()).digest()


def rp_id_hash(rp_id: str = TEST_RP["id"]) -> bytes:
    return hashlib.sha256(rp_id.encode()).digest()


def err_name(code: int) -> str:
    try:
        return CtapError.ERR(code).name
    except ValueError:
        return hex(code)


class Secret(str):
    """A PIN: prints as <PIN> in tracebacks and logs."""

    def __repr__(self):
        return "<PIN>"

    __str__ = __repr__

    def __add__(self, other):
        return Secret(str.__add__(self, other))


def read_secret(prompt: str, echo: bool = False) -> str:
    """Reads a line from the console, echoing '*' (or the text with `echo`).

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
                put(c if echo else "*")
        put("\r\n")
        text = "".join(chars)
        return text if echo else Secret(text)
    import termios
    with open("/dev/tty", "r+") as tty:
        tty.write(prompt)
        tty.flush()
        old = termios.tcgetattr(tty)
        new = termios.tcgetattr(tty)
        if not echo:
            new[3] &= ~termios.ECHO
        try:
            termios.tcsetattr(tty, termios.TCSADRAIN, new)
            line = tty.readline().rstrip("\n")
        finally:
            termios.tcsetattr(tty, termios.TCSADRAIN, old)
        tty.write("\n")
        return line if echo else Secret(line)


PIN_FATAL = (CtapError.ERR.PIN_INVALID, CtapError.ERR.PIN_AUTH_BLOCKED,
             CtapError.ERR.PIN_BLOCKED)
MIN_PIN_RETRIES = 4  # do not ask for the PIN below this


def confirm_reset():
    print("\n" + "=" * 60)
    print("  --reset: the reset tests ERASE ALL FIDO credentials and the PIN")
    print("  on the authenticator (other applets are not touched).")
    print("=" * 60)
    if read_secret("Type RESET to continue: ", echo=True) != "RESET":
        pytest.exit("--reset not confirmed", returncode=2)


def set_pin(ctap2, min_len):
    """Asks for a new PIN twice and sets it (the authenticator has no PIN)."""
    print("\n" + "=" * 60)
    print("  --set-pin: the authenticator has no PIN. Choose a new one")
    print(f"  ({min_len}..63 characters). Only a FIDO reset removes it.")
    print("  Empty input: cancel.")
    print("=" * 60)
    pin = read_secret("New PIN: ")
    if not pin:
        pytest.exit("--set-pin cancelled", returncode=2)
    if len(pin) < min_len:
        pytest.exit(f"PIN shorter than {min_len}: not set", returncode=2)
    if read_secret("Repeat PIN: ") != pin:
        pytest.exit("PINs differ: not set", returncode=2)
    ClientPin(ctap2).set_pin(pin)
    print("PIN set")
    return pin


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
        self.attach(device)
        self.pin = pin
        self._capman = capman

    def attach(self, device):
        """Switches to `device` (e.g. reopened after a replug), re-reads getInfo."""
        self.device = device
        try:
            self.ctap2 = Ctap2(device)
            self.info = self.ctap2.info
        except CtapError:  # no CTAP2: transport tests only
            self.ctap2 = self.info = None

    def say(self, text):
        with self._capman.global_and_fixture_disabled():
            print(text, flush=True)

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


@pytest.fixture
def device(auth):
    return auth.device  # current one: replug tests reopen it


@pytest.fixture(scope="session")
def auth(request):
    return request.config.fido_auth


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
