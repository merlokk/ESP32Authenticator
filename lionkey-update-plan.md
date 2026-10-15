# LionKey fork: update plan

Goal: move `lib/lionkey` (upstream `pokusew/lionkey`, CTAP 2.1, used as is)
to our fork and add the features below. Spec references: CTAP 2.2 PS and
CTAP 2.3 (Feb 2026, no breaking changes over 2.2). CBOR keys, permission bits
and error codes are taken from the spec when each step starts, not from
memory.

## 0. Fork and layout (done)

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
  it on the host: `host_test\lionkey.cmd` (WSL, gcc, ASan/UBSan; 130 tests
  pass on `c1518f2`).
  On the board: `tests/fido2` (pytest + python-fido2, `--transport ble`).
- `components/fido_ctap/CMakeLists.txt`: new core sources; `sources.md`:
  the fork URL.

Persistent state rule: new data goes into **new storage keys**
(`CTAP_STORAGE_KEY_CONFIG = 4`, ...), never into resized existing items
(`ctap_pin_persistent_state_t` is packed and size-asserted), so `fido.bin`
from the current firmware loads unchanged. New credential flags use free bits
of `ctap_credential_source_t.flags` (bits 8+).

## 1. Buttons: port from approver-esp32 (done)

Our `devices/buttons` is a raw level read. Port from
`../../ai-remote/approver-esp32/components/buttons`:

- `Debounce` (25 ms) and `PressLength` (long press fires at the threshold,
  while held): pure, into a header with host tests (port `test_buttons.cpp`
  to Unity in `host_test`).
- `HeldFor(hold_ms)` (blocking, polled), on `buttons::Button`.
- Kept `buttons::Button` + `boards::Button(ButtonId)` as the board-facing API;
  the debounced state sits behind it (`Poll()`, `Pressed()`, `StableMs()`).
  The multi-button `Buttons` table of approver-esp32 is not ported.
- `fido_ctap` presence: a fresh *debounced* press (was a raw edge).

## 2. Long touch for reset (2.3) (done)

- Core (fork `608da6e`): `ctap_state_t.long_touch_for_reset_ms` (0 = off, set
  by the platform, kept by `ctap_init()`); `ctap_reset()` then calls the
  platform `ctap_wait_for_long_touch(ms)` (weak default = ordinary touch);
  getInfo `longTouchForReset` (0x18) = true. `CTAP_LONG_TOUCH_FOR_RESET_MS`
  = 5000 (2.3; 2.2 said 10 s). getInfo key defines 0x16..0x1F added.
  Tests: `ctap_reset_test.cpp` (9).
- Ours: always on (no way to turn it off, so `enableLongTouchForReset`,
  config subcommand 4, is a no-op in step 3). Hold Left 5 s; LED yellow fast
  blink while waiting, solid while held; released early = start over; 30 s
  timeout / CANCEL as for presence.
- `transportsForReset` (0x1A) not sent: absent = every transport (BLE only).
- Kept the spec's "within 10 s of power-up" rule (no display); it counts from
  the core start (boot, or the PIN unlock with an encrypted store). Over BLE
  the host rarely makes it; `fido2 reset` on the console stays the practical
  way. Revisit with the e-ink UI (a display lifts the rule).
- Not checked on the board: a reset would wipe the real credentials; getInfo
  over BLE needs `tests/fido2` from an administrator terminal.
- Reset still wipes only the FIDO2 store (not the vault); the vault keeps its
  own wipe PIN.

## 3. authenticatorConfig (0x0D) and setMinPINLength (done)

- Core: `ctap_config.c`, `ctap_parse_config()` (CTAP 2.2 §6.11). Supported:
  `toggleAlwaysUv` (2, can be turned off again), `setMinPINLength` (3),
  `enableLongTouchForReset` (4, CTAP 2.3); others (enterprise attestation,
  vendorPrototype) -> `CTAP1_ERR_INVALID_PARAMETER` (the spec's code for an
  unsupported subcommand).
- Auth when a PIN is set or alwaysUv is on: pinUvAuthParam over
  `32 x 0xff || 0x0d || subCommand || subCommandParams` (raw bytes), token
  with `acfg`; toggleAlwaysUv skips it without a PIN (initial setup).
- Persistent: `CTAP_STORAGE_KEY_CONFIG` (4), `ctap_config_persistent_state_t`
  (flags: alwaysUv, forcePINChange, longTouchForReset); the minimum PIN length
  stays in the PIN item. Old `fido.bin` loads unchanged (no item = defaults);
  reset erases it.
- setMinPINLength: only raises (else PIN_POLICY_VIOLATION; also above 63);
  `minPinLengthRPIDs` -> INVALID_PARAMETER until step 4; `forceChangePin`
  without a PIN -> PIN_NOT_SET; a PIN below the new minimum sets forcePINChange;
  forcePINChange resets all pinUvAuthTokens; `pinComplexityPolicy` ignored
  until step 5.
- forcePINChange: getPinToken -> PIN_INVALID, getPinUvAuthTokenUsingPin... ->
  PIN_POLICY_VIOLATION (also when the PIN is below the minimum), without
  losing an attempt; changePIN to the same PIN -> PIN_POLICY_VIOLATION;
  a changePIN clears it.
- alwaysUv: makeCredential step 6 and getAssertion step 5 (`up` present and
  true) -> PUAT_REQUIRED without pinUvAuthParam; `makeCredUvNotRqd` reported
  false.
- getInfo: options `alwaysUv`, `authnrCfg`, `setMinPINLength` (canonical key
  order), `forcePINChange` (0x0C), `maxRPIDsForSetMinPINLength` (0x10) = 0,
  `longTouchForReset` (0x18) always present (true/false),
  `authenticatorConfigCommands` (0x1F) = [2, 3, 4].
- Reset window: `ctap_state_t.reset_window_ms` (0 = 10 s default,
  `CTAP_RESET_WINDOW_NONE`); ours from the board class `fido_reset_window_s`
  (10 on both boards, 0 = any time) via `fido_ctap_set_reset_window()`.
- Fixed in passing: `ctap_parse_config()` checks a missing request before
  entering the map (credentialManagement's parser dereferences it first).
- Tests: `ctap_config_test.cpp` (27), forcePINChange in
  `ctap_client_pin_test.cpp` (3); `tests/fido2/test_config.py` over BLE
  (never setMinPINLength on the device: only a reset lowers it again):
  `test_info.py` + `test_config.py` 15 passed, toggleAlwaysUv round trip
  skipped (`--destructive`), on the dev board 2026-10-15.
- Console: `fido2 info` shows min PIN length, change due, always uv, reset
  rule; `board` the reset window.

## 4. minPinLength extension (done)

- makeCredential `extensions.minPinLength: true` -> authData extension
  `minPinLength: <current minimum>` only for an RP ID in the list; otherwise
  omitted (and no ED flag when nothing else is output; before, an extensions
  map with only unsupported entries gave an empty map with ED).
- The list: setMinPINLength `minPinLengthRPIDs`, kept as SHA-256 of each RP
  ID (the request's rpId hash is compared) under
  `CTAP_STORAGE_KEY_MIN_PIN_LENGTH_RP_IDS` (5), up to
  `CTAP_MAX_MIN_PIN_LENGTH_RP_IDS` = 8 (getInfo 0x10). A non-empty list
  replaces the previous one; an empty one changes nothing; more than 8 ->
  KEY_STORE_FULL (spec names no code; nothing changes); a non-string ->
  CBOR_UNEXPECTED_TYPE. Reset clears it.
- getInfo `extensions`: + `minPinLength`.
- Tests: `ctap_config_test.cpp` (+6); `fido2 info` shows how many sites;
  over BLE `test_info.py` + `test_config.py` 16 passed, 1 skipped (2026-10-15).
- The list cannot be emptied except by a reset: not set on the device by
  `tests/fido2`.

## 5. PIN complexity policy (done)

- The rules: `components/pin_policy/pin_policy.h` (pure C, host-tested
  `test_pin_policy.cpp`): no block repeated (1111, 1212, 123123), no run up
  or down of digits or letters (1234, 9876, 7890, abcd; doubled: 112233), not
  on a short list of common PINs (2580, 147258, password, qwerty, ...);
  letters without case. Only a new PIN is checked; an existing one stays.
- Core (fork): `ctap_pin_meets_complexity_policy()` (weak default: one
  character repeated, a run of digits; ours: pin_policy.h); setPIN / changePIN
  -> PIN_POLICY_VIOLATION while the policy is on. On =
  `ctap_state_t.pin_complexity_policy_preconfigured` (also after a reset) or
  setMinPINLength `pinComplexityPolicy: true` (config flag, until a reset;
  false does not turn it off). getInfo `pinComplexityPolicy` (0x1B) always,
  `pinComplexityPolicyURL` (0x1C) when the platform gives one (we do not),
  `pinComplexityPolicy` in `extensions` while on. makeCredential extension
  `pinComplexityPolicy` -> bool for RP IDs in minPinLengthRPIDs (12.6).
- Ours: config.json `pin.complexity` (default true): the FIDO2 pre-configured
  value and the vault: a new vault PIN (new store, `pin change`, `pass reset
  confirm <pin>`) -> `kWeakPin`. `fido2 info` shows it.
- Tests: core +7 (`ctap_config_test.cpp`, `ctap_client_pin_test.cpp`); over
  BLE `test_info.py` + `test_config.py` 17 passed, 1 skipped (2026-10-15).

## 6. EdDSA (Ed25519, COSE -8 and -19) (done)

- Library: Monocypher 4.0.2 (`crypto/monocypher`, CC0 / BSD-2, Cure53
  audit), only Ed25519. mbedTLS 4.1 / TF-PSA-Crypto 1.1 in ESP-IDF v6.0.2
  has the PSA EdDSA constants but no implementation; no Ed25519 hardware.
- Crypto table: `ed25519_compute_public_key`, `ed25519_sign` (the message,
  not a hash). Optional: NULL (e.g. upstream's stm32 table) -> EdDSA is not
  offered (`ctap_crypto_supports_eddsa()`). Monocypher wipes the seed it is
  given: ours works on a copy (test).
- COSE `-8` EdDSA and `-19` Ed25519 (RFC 9864): the same key and signature;
  the credential keeps which one (`flags` bits 8..9; 0 = ES256, so old
  credentials stay ES256). makeCredential takes the first supported entry in
  the host's pubKeyCredParams (webauthn.io lists -8 first: Ed25519 there).
  getInfo `algorithms`: -7, -8, -19.
- COSE key OKP `{1: 1, 3: alg, -1: 6, -2: x}`; signatures (packed self
  attestation, assertions) over `authData || clientDataHash`, raw 64 bytes;
  attStmt `alg` = the credential's. Credential management lists the OKP key.
- `fido2 list` and the web FIDO2 page show the algorithm.
- Tests: `ctap_ed25519_test.cpp` (RFC 8032 vectors 1..3, end to end with
  Monocypher's verify), webauthn.io request now picks -8;
  `tests/fido2/test_eddsa.py` over BLE (one touch each, ctap-test.example):
  with test_info + test_config 19 passed, 1 skipped (2026-10-15).
- Flash: the app grew by ~24 KB (steps 3..6).

## 7. U2F (CTAP1) over BLE (done)

- Core: `ctap1.c` / `ctap1.h`, `ctap1_request()`: U2F_VERSION (`U2F_V2`),
  U2F_REGISTER, U2F_AUTHENTICATE (P1 `0x07` check-only -> 6985 for our key
  handle, `0x03` enforce, `0x08` don't enforce); short and extended APDUs;
  6700 / 6A80 / 6D00 / 6E00 / 6986 as the spec says.
- A U2F credential: non-discoverable ES256, `rp_id_hash` = the application
  parameter, no RP ID string, flag `u2f` (bit 10); key handle = the 32-byte
  credential ID. `ctap_credential_matches_rp()` compares only the hash for
  it, so CTAP2 getAssertion with rpId = the AppID (appid extension) finds it.
  CTAP2 ES256 credentials work with U2F_AUTHENTICATE (SHA-256 of the RP ID);
  EdDSA ones and credProtect 3 -> 6A80. Counter: the global one.
- User presence: blocks with KEEPALIVE (TUP_NEEDED) like CTAP2 (allowed over
  BLE; no 6985-and-retry), 6985 on timeout / cancel.
- Attestation: a per-authenticator P-256 key and a self-signed X.509 v3
  certificate (CN=LionKey U2F, 2026..2049, random serial), made at the first
  U2F_REGISTER, kept under `CTAP_STORAGE_KEY_U2F_ATTESTATION` (6); a reset
  makes a new one. Not batch attestation (sites checking the MDS see an
  unknown key).
- BLE (`devices/ble_fido`): `fidoServiceRevisionBitfield` reads 0x60 (U2F 1.2
  + FIDO2); the client writes one of them; MSG frames with CLA 0x00 go to
  `ctap1_request` (`fido_ctap_request`).
- `fido2 list` / web: a U2F credential's site is `U2F <app hash prefix>`.
- Tests: `ctap1_test.cpp` (10: register and its certificate verified with
  micro-ecc, the three authenticate modes, CTAP2 <-> U2F, reset);
  `tests/fido2/test_u2f.py` (python-fido2 `Ctap1`, two touches; the
  registration and certificate verified by python-fido2 / cryptography):
  with test_info + test_config 22 passed, 1 skipped (2026-10-15).
- Excluded: U2F over USB (no FIDO HID).

## 8. hmac-secret-mc (2.2) (done)

- makeCredential `hmac-secret-mc` (keyAgreement, saltEnc, saltAuth,
  pinUvAuthProtocol), only with `hmac-secret: true` (else MISSING_PARAMETER):
  the encrypted HMAC output(s) of the new credential's CredRandom (with / without
  UV as the response's uv flag) in the authData extensions (`hmac-secret-mc`,
  canonical order after `minPinLength`). WebAuthn PRF at create.
- getAssertion's hmac-secret code split into `prepare_hmac_secret()` (shared
  secret, saltAuth, salts) and `compute_hmac_secret_output()`, used by both.
- getInfo `extensions`: + `hmac-secret-mc`.
- Tests: `ctap_hmac_secret_mc_test.cpp` (4: the platform side with micro-ecc
  and tiny-AES; the makeCredential output equals getAssertion's for the same
  salt); `tests/fido2/test_hmac_secret_mc.py` (PIN/UV protocol 2, two touches):
  with test_info 8 passed (2026-10-15).

## 9. Read-only credential management (2.2/2.3) (done)

- Permission `pcmr` (0x40), only alone (else UNAUTHORIZED_PERMISSION), via
  getPinUvAuthTokenUsingPinWithPermissions: assigned to the
  persistentPinUvAuthToken, which is returned (the regular token is left as
  it is). getInfo option `perCredMgmtRO: true`.
- persistentPinUvAuthToken: one per PIN/UV protocol, made at the first pcmr
  request (lazily: no RNG use or storage write at boot), kept with its
  permissions under `CTAP_STORAGE_KEY_PERSISTENT_PUAT` (7), so it survives a
  power cycle and the same token comes back next time. No usage timer, no
  in-use state. resetPersistentPinUvAuthToken() (dropped; a new one at the
  next request): changePIN, setMinPINLength with forcePINChange, reset.
- credentialManagement: getCredsMetadata, enumerateRPsBegin,
  enumerateCredentialsBegin try verify(persistentPinUvAuthToken, ...) first
  (pcmr required, else PIN_AUTH_INVALID; no RP ID rules), then the regular
  token (cm). deleteCredential / updateUserInformation: the regular token only.
- Tests: `ctap_pcmr_test.cpp` (8: the platform side with micro-ecc /
  tiny-AES); `tests/fido2/test_pcmr.py` (python-fido2 `CredentialManagement`
  with the pcmr token: reads, delete refused, the same token again, pcmr only
  alone): with test_info + test_config 21 passed, 1 skipped (2026-10-15).
- Firmware: nothing beyond the core (storage key 7 is kept in `fido.bin`).

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
