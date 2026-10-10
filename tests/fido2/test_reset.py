"""authenticatorReset (0x07). Needs --reset: erases ALL FIDO credentials and the PIN.

CTAP 2.1 makes both guards optional (MAY): user presence and a time window
after power-up (YubiKey: a touch, within 10 s of plug-in). Flow: make
credentials, replug, reset at once, check that everything is gone.
"""

import threading
import time

import pytest
from fido2.ctap import CtapError

import transports
from conftest import STATUS_UPNEEDED, client_data_hash, expect_error

ERR = CtapError.ERR
pytestmark = pytest.mark.reset

RESET_RP = {"id": "ctap-test-reset.example", "name": "CTAP reset test"}
RESET_USER = {"id": b"ctap-test-reset-user", "name": "reset", "displayName": "Reset"}
WINDOW_PROBE_DELAY = 12.0  # s after plug-in; YubiKey's window is 10 s


def descriptor(att):
    return {"type": "public-key", "id": att.auth_data.credential_data.credential_id}


@pytest.fixture(scope="module")
def before(auth):
    """Credentials made before the reset: a plain one and, if possible, a
    discoverable one."""
    if auth.pin_needed_for_make_credential() and not auth.pin:
        pytest.skip("the authenticator requires a PIN for makeCredential")
    creds = {"plain": auth.make_credential(client_data_hash("reset-plain"),
                                           rp=RESET_RP, user=RESET_USER)}
    if auth.info.options.get("rk") and (auth.pin or not auth.info.options.get("clientPin")):
        creds["rk"] = auth.make_credential(client_data_hash("reset-rk"), rp=RESET_RP,
                                           user=RESET_USER, rk=True)
    return creds


@pytest.fixture(scope="module")
def plugged_at(request, auth, before):
    """Replug, reset at once; returns the plug-in time (monotonic)."""
    auth.say("\n" + "=" * 60 +
             "\n  RESET: unplug the authenticator and plug it back in,"
             "\n  then touch it as soon as it blinks (YubiKey: within 10 s)."
             "\n" + "=" * 60)
    device = transports.wait_replug(auth.device, request.config.getoption("--device"))
    t = time.monotonic()
    auth.attach(device)
    auth.say(">>> plugged in: touch the authenticator now")
    auth._prompted = True  # the prompt above replaces the keepalive one
    try:
        auth.ctap2.reset(on_keepalive=auth.on_keepalive)
    except CtapError as e:
        if e.code == ERR.NOT_ALLOWED:
            pytest.fail("reset refused (NOT_ALLOWED): too late after plug-in, "
                        "run again and touch sooner")
        raise
    auth.attach(device)  # getInfo after the reset
    auth.pin = None
    return t


def test_pin_cleared(auth, plugged_at):
    if "clientPin" in auth.info.options:
        assert auth.info.options["clientPin"] is False


def test_credential_gone(auth, plugged_at, before):
    expect_error(ERR.NO_CREDENTIALS, auth.get_assertion, client_data_hash("gone"),
                 rp_id=RESET_RP["id"], allow_list=[descriptor(before["plain"])],
                 up=False)


def test_discoverable_gone(auth, plugged_at, before):
    if "rk" not in before:
        pytest.skip("no discoverable credential was made before the reset")
    expect_error(ERR.NO_CREDENTIALS, auth.get_assertion, client_data_hash("rk-gone"),
                 rp_id=RESET_RP["id"], up=False)


def test_new_credential(auth, plugged_at):
    cdh = client_data_hash("after-reset")
    att = auth.make_credential(cdh, rp=RESET_RP, user=RESET_USER)
    a = auth.get_assertion(cdh, rp_id=RESET_RP["id"], allow_list=[descriptor(att)],
                           up=False)
    a.verify(cdh, att.auth_data.credential_data.public_key)


def test_reset_window(auth, plugged_at):
    """A reset long after plug-in is refused with NOT_ALLOWED.

    Optional in CTAP. If the authenticator asks for a touch instead, the
    request is cancelled at once, so nothing is erased.
    """
    time.sleep(max(0.0, plugged_at + WINDOW_PROBE_DELAY - time.monotonic()))
    cancel = threading.Event()

    def on_keepalive(status):
        if status == STATUS_UPNEEDED:
            cancel.set()

    try:
        auth.ctap2.reset(event=cancel, on_keepalive=on_keepalive)
    except CtapError as e:
        if e.code == ERR.NOT_ALLOWED:
            return
        if e.code == ERR.KEEPALIVE_CANCEL:
            pytest.skip("no power-up window: reset waited for a touch (cancelled)")
        raise
    pytest.fail("reset accepted long after power-up without a touch")
