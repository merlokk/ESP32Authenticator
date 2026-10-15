# LionKey fork: update plan

Goal: move `lib/lionkey` (upstream `pokusew/lionkey`, CTAP 2.1, used as is)
to our fork and add the features below. Spec references: CTAP 2.2 PS and
CTAP 2.3 (Feb 2026, no breaking changes over 2.2). CBOR keys, permission bits
and error codes are taken from the spec when each step starts, not from
memory.

## 0. Fork and layout

- Fork: `github.com/merlokk/lionkey` (done; remote `upstream` = pokusew).
  Branch `esp32auth` from the current
  submodule commit (`c1518f2`); `.gitmodules` points to the fork. `main` of the
  fork follows upstream; our branch is rebased on it.
- Commits in the fork: one feature per commit, upstream style (tabs,
  `ctap_*` names, spec step comments), so each can go upstream as a PR.
- Board-specific code stays here (`components/fido_ctap`): hooks, crypto
  binding, storage. The core gets only generic hooks with weak/NULL defaults.
- Tests: LionKey's gtest suite (`lib/lionkey/test`) gets a test per feature
  (it already covers getInfo, makeCredential, getAssertion, clientPIN); build
  it on the host next to `host_test\run.cmd` (add `host_test\lionkey.cmd`).
  On the board: `tests/fido2` (pytest + python-fido2, `--transport ble`).
- `components/fido_ctap/CMakeLists.txt`: new core sources; `sources.md`:
  the fork URL.

Persistent state rule: new data goes into **new storage keys**
(`CTAP_STORAGE_KEY_CONFIG = 4`, ...), never into resized existing items
(`ctap_pin_persistent_state_t` is packed and size-asserted), so `fido.bin`
from the current firmware loads unchanged. New credential flags use free bits
of `ctap_credential_source_t.flags` (bits 8+).

## 1. Buttons: port from approver-esp32

Our `devices/buttons` is a raw level read. Port from
`../../ai-remote/approver-esp32/components/buttons`:

- `Debounce` (25 ms) and `PressLength` (long press fires at the threshold,
  while held): pure, into a header with host tests (port `test_buttons.cpp`
  to Unity in `host_test`).
- `Buttons::HeldFor(index, hold_ms)` (blocking, polled).
- Keep `buttons::Button` + `boards::Button(ButtonId)` as the board-facing API;
  the debounced state sits behind it.
- `fido_ctap` presence: a fresh *debounced* press (today a raw edge).

## 2. Long touch for reset (2.3)

- Core: `ctap_reset()` calls a new platform hook
  `ctap_wait_for_long_touch()` (weak default = `ctap_wait_for_user_presence()`).
- getInfo: `longTouchForReset: true` (2.3 field), `transportsForReset: ["ble"]`
  (2.2 field) if we keep reset on BLE only.
- `authenticatorConfig` `enableLongTouchForReset` (2.3) turns it on; until
  then reset works as today.
- Ours: hold Left >= 5 s (the 2.3 duration); LED yellow fast blink while waiting, solid yellow
  once held long enough, then reset. Cancel / 30 s timeout as for presence.
- Open question (read 2.3 first): drop the "within 10 s of power-up" rule when
  long touch is used. Over BLE the host rarely reconnects within 10 s, so
  today `fido2 reset` from the console is the only practical reset.
- Reset still wipes only the FIDO2 store (not the vault); the vault keeps its
  own wipe PIN.

## 3. authenticatorConfig (0x0D) and setMinPINLength

- Dispatch `CTAP_CMD_CONFIG` (enum exists, no handler). New `ctap_config.c`.
- Auth: pinUvAuthParam over `32 x 0xff || 0x0d || subCommand ||
  subCommandParams` with the `acfg` permission; without a PIN set, allowed
  only per spec (UP).
- Subcommands:
  - `setMinPINLength`: `newMinPINLength` (only raises), `minPinLengthRPIDs`,
    `forceChangePin`. Stored under `CTAP_STORAGE_KEY_CONFIG`
    (min length, force-change flag, RP ID list, alwaysUv).
  - `toggleAlwaysUv`: stored; enforced in makeCredential / getAssertion
    (UV or PIN token required, `makeCredUvNotRqd` reported false).
  - `enableEnterpriseAttestation`: not supported (`ep` absent).
  - `vendorPrototype`: not supported.
- getInfo: `authnrCfg`, `setMinPINLength`, `alwaysUv` options;
  `minPINLength`, `forcePINChange`, `maxRPIDsForSetMinPINLength`.
- forcePINChange: getPinToken / getPinUvAuthTokenUsingPinWithPermissions
  return `CTAP2_ERR_PIN_POLICY_VIOLATION` until changePIN; changePIN clears it.
- `reset` clears the config item.
- Console: `fido2 info` shows min PIN length, forceChangePin, alwaysUv.

## 4. minPinLength extension

- makeCredential `extensions.minPinLength: true` -> authData extension with
  the current minimum, only for RP IDs in `minPinLengthRPIDs`.
- getInfo `extensions`: add `minPinLength`.

## 5. PIN complexity policy (2.3)

- setPIN / changePIN: besides the length, reject weak PINs with
  `CTAP2_ERR_PIN_POLICY_VIOLATION`: all one character, ascending/descending
  runs (`123456`, `abcdef`), a short built-in list of the most common PINs.
- The check is a pure function (`pin_policy.h`, host-tested), shared with the
  vault's 6-digit PIN (console `pin set`, web).
- getInfo: `pinComplexityPolicy` in `extensions` and the
  `pinComplexityPolicy` / `pinComplexityPolicyURL` fields, only once the check
  is on.
- Policy on/off: `config.json` `fido.pinPolicy` (default on).

## 6. EdDSA (Ed25519, COSE alg -8)

- Crypto interface (`ctap_crypto.h`): `ed25519_compute_public_key`,
  `ed25519_sign` (message, not hash). Ours: Monocypher (one .c, CC0/BSD) in
  `components/fido_ctap`; mbedTLS/PSA in ESP-IDF has no Ed25519. Test vectors
  from RFC 8032 in the gtest suite.
- Credential: flag bit `CTAP_CREDENTIAL_SOURCE_CTAP_flags_alg_eddsa`; the
  32-byte `private_key` holds the Ed25519 seed. Old credentials (bit clear)
  stay ES256.
- makeCredential: pick the first host `pubKeyCredParams` entry we support
  (today: check that ES256 is present).
- `ctap_credential_compute_signature`: take the message
  (`authData || clientDataHash`) and hash only for ES256. Signature: 64 raw
  bytes for EdDSA, DER for ES256.
- COSE key: OKP (`kty 1, crv 6, x`) for EdDSA, EC2 for ES256
  (`ctap_encode.c`, `ctap_credentials.c`).
- Self attestation (`packed`) signs with the credential's own algorithm.
- getInfo `algorithms`: ES256 first, then EdDSA.
- `fido2 list`: show the algorithm.

## 7. U2F (CTAP1) over BLE

- Core: new `ctap1.c`: `ctap1_request(apdu) -> response + SW`, with
  `U2F_VERSION` (`U2F_V2`), `U2F_REGISTER`, `U2F_AUTHENTICATE`
  (P1 `0x03` enforce-UP, `0x07` check-only, `0x08` dont-enforce).
- Key handle = our 32-byte credential ID; the credential is stored as
  non-discoverable with `rp_id_hash = appParam` and flag `u2f` (no RP ID
  string; console shows `(U2F) <hash prefix>`). CTAP2 getAssertion with this
  ID and the appid extension (rpIdHash = SHA-256(appid)) finds it too.
- Counter: the global signature counter (NVS), as CTAP2.
- User presence: U2F returns `SW_CONDITIONS_NOT_SATISFIED` (0x6985) and the
  host retries. A press within 10 s of the first try is latched for one
  request (LED yellow blink meanwhile).
- Attestation certificate for REGISTER: a self-signed X.509 for a per-device
  key, generated at first use, stored as `CTAP_STORAGE_KEY_U2F_ATTESTATION`
  (DER built with `ctap_asn1`). Not batch attestation: sites that check the
  FIDO MDS see an unknown authenticator (same as today for CTAP2).
- Transport (`devices/ble_fido`): advertise revisions FIDO2 + U2F 1.2 in
  `fidoServiceRevisionBitfield`; route MSG frames with CLA `0x00` to
  `ctap1_request` (today `DefaultHandler` answers `0x6D00`).
- Excluded: U2F over USB (no FIDO HID), `U2F_AUTHENTICATE` with a key handle
  of another RP (answers `0x6A80`).

## 8. hmac-secret-mc (2.2)

- makeCredential: `hmac-secret-mc` input (keyAgreement, saltEnc, saltAuth,
  pinUvAuthProtocol) as in getAssertion; needs `hmac-secret: true` in the
  same request. Output: encrypted HMAC output(s) in the authData extensions,
  with the credential's `cred_random_with_uv/without_uv` just created.
- Reuse the getAssertion hmac-secret code (factor out the decrypt/compute/
  encrypt part into one function).
- getInfo `extensions`: add `hmac-secret-mc`.

## 9. Read-only credential management (2.2/2.3)

- New permission `pcmr` (per-credential management read-only): allows
  `getCredsMetadata`, `enumerateRPs*`, `enumerateCredentials*` only;
  `deleteCredential` and `updateUserInformation` need `cm`.
- Persistent pinUvAuthToken: a token with only `pcmr` survives the usual
  expiry (UP timeout / usage window) until reset, PIN change or reboot (per
  spec), so the OS lists keys without asking the PIN each time.
- getInfo: the read-only option (2.3 `perCredMgmtRO`) and
  `pinUvAuthToken` permissions as the spec lists them.

## 10. Attestation formats (2.2)

- makeCredential `attestationFormatsPreference`: honour `none` (empty
  `attStmt`, no signature, fmt `none`) and `packed` (self, as today).
- getInfo `attestationFormats: ["packed", "none"]`.
- Default without a preference: `packed` (unchanged).

## 11. encIdentifier / encCredStoreState in getInfo (2.2)

- Both need the persistent pinUvAuthToken from step 9: values are encrypted
  with a key derived from it (HKDF, per spec), so only a host holding that
  token can read them.
- `encIdentifier`: a random 16-byte device identifier, made once and kept in
  NVS (`fido/dev_id`; not the MAC: privacy). Cleared on reset.
- `encCredStoreState`: a random value replaced on every credential create,
  delete or user-info update (the store version we already keep in NVS can
  drive it). Useful later for our own Wi-Fi sync between devices.

## 12. Built-in UV (PIN on the device): plumbing only

- Core: `clientPIN` subcommand `getPinUvAuthTokenUsingUvWithPermissions`,
  `getUVRetries`, the platform hook `ctap_perform_built_in_uv()`; UV retries
  in `CTAP_STORAGE_KEY_CONFIG`.
- Now: the hook **only logs** (`fido_ctap: built-in UV requested (rp ...)`)
  and returns "not verified"; getInfo `uv: false` (supported, not
  configured), so hosts fall back to clientPIN. Never treat the button as UV.
- Later: the X4 Pro enters the vault PIN on the e-ink screen with buttons;
  `uv: true` once the screen exists. Also `uvModality` (passcode),
  `preferredPlatformUvAttempts`.

## Order

| # | Step | Depends on |
|---|---|---|
| 0 | Fork, gtest build on the host | |
| 1 | Buttons port | |
| 2 | Long touch for reset | 1 |
| 3 | authenticatorConfig, setMinPINLength, alwaysUv | 0 |
| 4 | minPinLength extension | 3 |
| 5 | PIN complexity policy | 3 |
| 6 | EdDSA | 0 |
| 7 | U2F over BLE | 0 |
| 8 | hmac-secret-mc | 0 |
| 9 | Read-only cred management, persistent token | 3 |
| 10 | Attestation formats | 0 |
| 11 | encIdentifier / encCredStoreState | 9 |
| 12 | Built-in UV plumbing | 3 |
| 13 | getInfo `versions`: add `FIDO_2_3` (see below) | |

Each step: core change + gtest, firmware glue, `tests/fido2` case, flash and
check with Chrome on Windows 11 over BLE (webauthn.io), docs
(`architecture.md`, `build.md` console commands, CLAUDE.md status).

## What CTAP 2.3 needs

There is no `FIDO_2_2` version string; a 2.2/2.3 authenticator reports
`FIDO_2_3`, and 2.3 adds no mandatory feature over 2.2. CTAP 2.2 §9
(Mandatory features) for an authenticator listing that version:

| Requirement | Today |
|---|---|
| hmac-secret extension | done |
| `rk` true -> clientPIN and/or built-in UV | done (clientPIN) |
| `rk` true -> `credMgmt` true (or a built-in UI) | done |
| credProtect when UV is supported | done |
| `pinUvAuthToken` true when `clientPin` or `uv` is true | done |
| PIN/UV auth protocol 2 in `pinUvAuthProtocols` | done (1 and 2) |
| Reset: `authenticatorReset` on at least one transport, or another way | done (BLE, console `fido2 reset`) |

So **none of steps 1-12 is required** for `FIDO_2_3`: step 13 can go first,
after a check that the 2.2 changes to existing commands are followed (new
getInfo fields only when supported, error codes; the FIDO conformance tool
over BLE, `tests/fido2`). Each step above is optional; the spec ties some of
them together:

| Step | Status in 2.2/2.3 |
|---|---|
| 2 Long touch for reset | optional; 5 s in 2.3 |
| 3 authenticatorConfig | optional (`authnrCfg`) |
| 4 minPinLength extension | optional; required for `minPinLengthRPIDs` in step 3 |
| 5 PIN complexity policy | optional (any extra PIN constraints are allowed) |
| 6 EdDSA | optional |
| 7 U2F | SHOULD (CTAP2 authenticators should also implement CTAP1/U2F) |
| 8 hmac-secret-mc | optional; needs hmac-secret |
| 9 Read-only cred management | optional |
| 10 Attestation formats | optional; if several are supported the host's preference order is a MUST |
| 11 encIdentifier / encCredStoreState | optional; needs step 9 |
| 12 Built-in UV | optional |

Detect 2.2 features by their getInfo fields, not by the version string.
