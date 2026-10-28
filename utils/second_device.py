"""A second device on the authentication server, for testing the sync.

It speaks the device protocol (ESP32AuthenticatorServer/device-protocol.md)
as the firmware does: registers with its own P-256 key and TOTP secret,
signs in with a client assertion, and reads and writes the account's stores.

  python utils/second_device.py register you@example.org   # open a window on the server first
  python utils/second_device.py list                       # the account's stores, every seed phrase
  python utils/second_device.py edit passwords             # a newer valid copy (asks the seed phrase and PIN)
  python utils/second_device.py edit passwords --new-pin   # ... under another PIN (a PIN change here)
  python utils/second_device.py corrupt passkeys           # a newer copy that does not decrypt
  python utils/second_device.py code                       # this device's TOTP code

`edit` downloads the newest copy of the store under the seed phrase, opens
it with the phrase and the PIN (asked, never stored or logged), and uploads
the same records sealed again (a new nonce, version + 1): for the board a
write on another device. `corrupt` needs no secrets: the newest copy with its
version raised, so the tag no longer matches (the board must refuse it).

The device's key, TOTP secret and client id live in a JSON file outside the
repo (--state, default ~/.esp32auth/second_device.json). Needs `cryptography`.
"""

import argparse
import base64
import getpass
import hashlib
import hmac
import json
import os
import pathlib
import re
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

REPO = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_STATE = pathlib.Path.home() / ".esp32auth" / "second_device.json"
KINDS = {"passkeys": 1, "passwords": 2}
KIND_NAMES = {v: k for k, v in KINDS.items()}

# --- encodings -------------------------------------------------------------------


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def b32(data: bytes) -> str:
    return base64.b32encode(data).rstrip(b"=").decode()


def jwt(key: ec.EllipticCurvePrivateKey, header: str, payload: dict) -> str:
    """header.payload.signature, ES256 (r || s), compact JSON as the firmware."""
    body = json.dumps(payload, separators=(",", ":"))
    signing_input = f"{b64url(header.encode())}.{b64url(body.encode())}"
    r, s = decode_dss_signature(key.sign(signing_input.encode(), ec.ECDSA(hashes.SHA256())))
    return f"{signing_input}.{b64url(r.to_bytes(32, 'big') + s.to_bytes(32, 'big'))}"


def totp(secret: bytes, now: float) -> str:
    mac = hmac.new(secret, struct.pack(">Q", int(now) // 30), hashlib.sha1).digest()
    offset = mac[-1] & 0x0F
    value = struct.unpack(">I", mac[offset : offset + 4])[0] & 0x7FFFFFFF
    return f"{value % 1000000:06d}"


# --- the store file (components/storefile/store_header.h) --------------------------

HEADER_SIZE = 80
TAG_SIZE = 16


class Header:
    """The 80-byte plain header: the AES-GCM associated data."""

    def __init__(self, raw: bytes):
        if len(raw) < HEADER_SIZE or raw[:4] != b"ESAF" or raw[4] != 3:
            raise ValueError("not a store file (magic ESAF, format 3)")
        self.kind = raw[5]
        self.cipher = raw[6]
        self.version, self.length = struct.unpack_from("<II", raw, 8)
        (self.modified,) = struct.unpack_from("<q", raw, 16)
        self.nonce = raw[24:36]
        (self.iterations,) = struct.unpack_from("<I", raw, 36)
        self.salt = raw[40:56]
        self.key_id = raw[56:64]
        self.store_id = raw[64:72]
        self.seed_id = raw[72:76]

    def encode(self) -> bytes:
        out = bytearray(HEADER_SIZE)
        out[0:5] = b"ESAF\x03"
        out[5] = self.kind
        out[6] = self.cipher
        struct.pack_into("<IIq", out, 8, self.version, self.length, self.modified)
        out[24:36] = self.nonce
        struct.pack_into("<I", out, 36, self.iterations)
        out[40:56] = self.salt
        out[56:64] = self.key_id
        out[64:72] = self.store_id
        out[72:76] = self.seed_id
        return bytes(out)


# --- the key (components/vault/vault.h) ----------------------------------------------


def words() -> list:
    """The BIP39 English list, from the firmware's copy."""
    text = (REPO / "components" / "vault" / "seed_words.cpp").read_text(encoding="utf-8")
    found = re.findall(r'"([a-z]+)"', text[text.index("kWords") :])
    if len(found) != 2048:
        raise SystemExit("seed_words.cpp: not 2048 words")
    return found


def entropy_of(phrase: str) -> bytes:
    """12 words (any case, a word cut to its first 4+ letters) -> 16 bytes, checksum checked."""
    table = words()
    given = phrase.lower().split()
    if len(given) != 12:
        raise SystemExit("the seed phrase is 12 words")
    bits = 0
    for w in given:
        matches = [i for i, full in enumerate(table) if full == w or (len(w) >= 4 and full.startswith(w))]
        if len(matches) != 1:
            raise SystemExit("a seed word is not in the list")
        bits = bits << 11 | matches[0]
    entropy = (bits >> 4).to_bytes(16, "big")
    if hashlib.sha256(entropy).digest()[0] >> 4 != bits & 0x0F:
        raise SystemExit("the seed phrase has a typo (its checksum)")
    return entropy


def store_key(pin: str, salt: bytes, iterations: int, entropy: bytes) -> tuple:
    """HKDF-SHA256(salt = the seed, PBKDF2-HMAC-SHA256(PIN, salt)), and its id."""
    stretched = PBKDF2HMAC(hashes.SHA256(), 32, salt, iterations).derive(pin.encode())
    key = HKDF(hashes.SHA256(), 32, entropy, b"ESAF store key").derive(stretched)
    return key, hashlib.sha256(b"ESAF key id" + key).digest()[:8]


# --- the server ------------------------------------------------------------------------


class Server:
    def __init__(self, host: str, state_path: pathlib.Path):
        self.host = host
        self.state_path = state_path
        self.state = json.loads(state_path.read_text()) if state_path.exists() else {}
        self.token = None

    def save(self):
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps(self.state, indent=1))

    def key(self) -> ec.EllipticCurvePrivateKey:
        return serialization.load_pem_private_key(self.state["key"].encode(), None)

    def request(self, method: str, path: str, body: bytes = None, content_type: str = None,
                auth: bool = True) -> tuple:
        """(status, body bytes); errors are answers too."""
        headers = {}
        if content_type:
            headers["Content-Type"] = content_type
        if auth:
            headers["Authorization"] = f"Bearer {self.sign_in()}"
        req = urllib.request.Request(f"https://{self.host}{path}", data=body, method=method,
                                     headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=20) as answer:
                return answer.status, answer.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def register(self, email: str):
        key = ec.generate_private_key(ec.SECP256R1())
        secret = os.urandom(20)
        public = key.public_key().public_bytes(serialization.Encoding.X962,
                                               serialization.PublicFormat.UncompressedPoint)
        mac = bytes([0x02]) + os.urandom(5)  # locally administered
        token = jwt(key, '{"alg":"ES256","typ":"JWT"}', {
            "email": email,
            "serial": os.urandom(16).hex(),
            "mac": ":".join(f"{b:02x}" for b in mac),
            "totp": b32(secret),
            "pub": b64url(public),
            "iat": int(time.time()),
        })
        status, body = self.request("POST", "/register", token.encode(), "application/jwt", auth=False)
        if status // 100 != 2:
            raise SystemExit(f"register: HTTP {status} {body.decode(errors='replace')}")
        self.state = {
            "host": self.host,
            "key": key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                     serialization.NoEncryption()).decode(),
            "totp": b32(secret),
            "clientId": json.loads(body)["clientId"],
        }
        self.save()
        print(f"registered: client id {self.state['clientId']}")

    def sign_in(self) -> str:
        if self.token:
            return self.token
        if "clientId" not in self.state:
            raise SystemExit("not registered: second_device.py register <email>")
        cid = self.state["clientId"]
        now = int(time.time())
        assertion = jwt(self.key(), '{"alg":"ES256","typ":"client-authentication+jwt"}', {
            "iss": cid, "sub": cid, "aud": f"https://{self.host}/", "jti": os.urandom(16).hex(),
            "iat": now, "exp": now + 300,
        })
        form = urllib.parse.urlencode({
            "grant_type": "client_credentials",
            "client_id": cid,
            "client_assertion_type": "urn:ietf:params:oauth:client-assertion-type:jwt-bearer",
            "client_assertion": assertion,
        }).encode()
        status, body = self.request("POST", "/connect/token", form,
                                    "application/x-www-form-urlencoded", auth=False)
        if status != 200:
            raise SystemExit(f"sign-in: HTTP {status} {body.decode(errors='replace')}")
        self.token = json.loads(body)["access_token"]
        return self.token

    def stores(self, seed_id: str = None) -> list:
        path = "/api/device/stores" + (f"?seedId={seed_id}" if seed_id else "")
        status, body = self.request("GET", path)
        if status != 200:
            raise SystemExit(f"stores: HTTP {status} {body.decode(errors='replace')}")
        return json.loads(body)["stores"]

    def download(self, s: dict) -> bytes:
        status, body = self.request(
            "GET", f"/api/device/stores/files/{s['storeId']}/{s['seedId']}/{s['kind']}?above=0")
        if status != 200:
            raise SystemExit(f"download: HTTP {status} {body.decode(errors='replace')}")
        return body

    def upload(self, data: bytes):
        status, body = self.request("POST", "/api/device/stores/files", data,
                                    "application/octet-stream")
        print(f"upload: HTTP {status} {body.decode(errors='replace')}")
        if status != 200:
            raise SystemExit(1)


def newest(stores: list, kind: int, store_id: str = None) -> dict:
    """The newest sync of `kind` (the server lists them so), or the one named."""
    for s in stores:
        if s["kind"] == kind and (store_id is None or s["storeId"] == store_id):
            return s
    raise SystemExit(f"no {KIND_NAMES[kind]} store on the server")


# --- commands --------------------------------------------------------------------------


def cmd_list(server: Server, args):
    for s in server.stores(args.seed_id):
        print(f"{KIND_NAMES.get(s['kind'], s['kind']):9} store {s['storeId']} seed {s['seedId']} "
              f"v{s['version']} key {s['keyId']} {s['length']} B, by {s.get('uploadedByDeviceId')} "
              f"at {s.get('uploadedAt')}")


def cmd_edit(server: Server, args):
    phrase = os.environ.get("ESP32AUTH_SEED") or getpass.getpass("seed phrase (12 words): ")
    entropy = entropy_of(phrase)
    seed_id = hashlib.sha256(entropy).digest()[:4].hex()
    s = newest(server.stores(seed_id), KINDS[args.kind], args.store)
    data = server.download(s)
    h = Header(data)
    pin = os.environ.get("ESP32AUTH_PIN") or getpass.getpass("PIN of that copy: ")
    key, key_id = store_key(pin, h.salt, h.iterations, entropy)
    if key_id != h.key_id:
        raise SystemExit("wrong PIN (or seed phrase) for that copy")
    plain = AESGCM(key).decrypt(h.nonce, data[HEADER_SIZE:], data[:HEADER_SIZE])
    if args.new_pin:
        new = getpass.getpass("the new PIN (6 digits): ")
        if not re.fullmatch(r"\d{6}", new):
            raise SystemExit("the PIN is 6 digits")
        h.salt = os.urandom(16)
        key, h.key_id = store_key(new, h.salt, h.iterations, entropy)
    h.version += 1
    h.modified = int(time.time())
    h.nonce = os.urandom(12)
    h.length = len(plain)
    head = h.encode()
    print(f"{args.kind}: v{h.version - 1} -> v{h.version}"
          f"{', another PIN' if args.new_pin else ''} ({len(plain)} B of records)")
    server.upload(head + AESGCM(key).encrypt(h.nonce, plain, head))


def cmd_corrupt(server: Server, args):
    s = newest(server.stores(args.seed_id), KINDS[args.kind], args.store)
    data = server.download(s)
    h = Header(data)
    h.version += 1  # the header is the associated data: the tag no longer matches
    print(f"{args.kind}: v{h.version - 1} -> v{h.version}, the tag left as it was")
    server.upload(h.encode() + data[HEADER_SIZE:])


def cmd_code(server: Server, args):
    print(totp(base64.b32decode(server.state["totp"] + "=" * (-len(server.state["totp"]) % 8)),
               time.time()))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=None, help="the server (default: the registered one, or auth.merlokk.com)")
    p.add_argument("--state", type=pathlib.Path, default=DEFAULT_STATE, help="this device's key file")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("register", help="a new key and TOTP secret, registered (open a window first)")
    r.add_argument("email")
    ls = sub.add_parser("list", help="the account's stores")
    ls.add_argument("--seed-id", help="only this seed phrase's (8 hex)")
    e = sub.add_parser("edit", help="a newer valid copy (the seed phrase and PIN asked)")
    e.add_argument("kind", choices=KINDS)
    e.add_argument("--store", help="the store id (default: the newest of the kind)")
    e.add_argument("--new-pin", action="store_true", help="sealed under another PIN")
    c = sub.add_parser("corrupt", help="a newer copy whose tag does not match")
    c.add_argument("kind", choices=KINDS)
    c.add_argument("--store", help="the store id (default: the newest of the kind)")
    c.add_argument("--seed-id", help="the seed phrase's (8 hex)")
    sub.add_parser("code", help="this device's TOTP code")
    args = p.parse_args()

    state = json.loads(args.state.read_text()) if args.state.exists() else {}
    server = Server(args.host or state.get("host") or "auth.merlokk.com", args.state)
    {"register": lambda: server.register(args.email), "list": lambda: cmd_list(server, args),
     "edit": lambda: cmd_edit(server, args), "corrupt": lambda: cmd_corrupt(server, args),
     "code": lambda: cmd_code(server, args)}[args.cmd]()


if __name__ == "__main__":
    sys.exit(main())
