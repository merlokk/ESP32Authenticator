# Store file format

`vault.bin` (passwords) and `fido.bin` (FIDO2 / U2F): a plain header, then
the encrypted payload. In `/sdcard/.authenticator` on the X4 Pro, else
`/spiffs`. Why this design: [storage-encryption.md](storage-encryption.md).
Code: `components/storefile` (header, I/O), `components/vault` (key, records),
`components/fido_ctap/ctap_file_storage.c` (fido.bin). The device's own
files on SPIFFS, plain (never synced): [seed.txt](#seedtxt),
[server.bin](#serverbin).

```
| header (80) | ciphertext (length) | GCM tag (16) |
```

## Header

Little-endian, 80 bytes (`store_header.h`, format 3). Authenticated: the whole
header is the AES-GCM associated data.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `ESAF` |
| 4 | 1 | format | 3 |
| 5 | 1 | kind | 1 `fido.bin`, 2 `vault.bin`; a file of another kind is refused |
| 6 | 1 | cipher | 0 plain, 1 AES-256-GCM (both stores: always 1, plain is refused) |
| 7 | 1 | reserved | 0 |
| 8 | 4 | version | +1 on every write, above the file's and the one kept in NVS |
| 12 | 4 | length | payload bytes (without the tag) |
| 16 | 8 | modified | Unix seconds (int64); 0 when the clock was not set |
| 24 | 12 | nonce | AES-GCM, random on every write |
| 36 | 4 | KDF iterations | PBKDF2-HMAC-SHA256 count (6000 for a new key) |
| 40 | 16 | KDF salt | random for every new key |
| 56 | 8 | key id | SHA-256(`"ESAF key id"` ‖ key)[0..8] |
| 64 | 8 | store id | random when the store is made; kept by every write and PIN change; the same on synced devices |
| 72 | 4 | seed id | SHA-256(seed entropy)[0..4]: which seed phrase is in the key |
| 76 | 4 | reserved | 0 |

File size must be 80 + `length` + 16.

## Key

```
stretched = PBKDF2-HMAC-SHA256(PIN (6 ASCII digits), salt, iterations, 32 bytes)
key       = HKDF-SHA256(salt = seed entropy (16 bytes), IKM = stretched,
                        info = "ESAF store key", 32 bytes)
```

- One key for both stores. The seed is the 12-word BIP39 phrase in
  `/spiffs/seed.txt` (the words as text), never next to the stores.
- Salt and iterations come from the header: another device with the same PIN
  and phrase derives the same key from the file.
- A PIN or seed change makes a new key (a **new random salt**, so a new key id
  even for the same PIN) and rewrites both files.
- A wrong PIN: the GCM tag fails. The key id tells "another key" without a
  decryption attempt (sync: the server's copy under another PIN).

## vault.bin payload

`records.h`, format 2, in list order (the user's):

```
format (1) = 2 | next id (4) | count (2) | record × count
record: id (4) | modified (8, Unix s, 0 unknown) |
        site | user | password | otp      each: length (1) + UTF-8 bytes
```

| Field | Max bytes |
|---|---|
| site, user | 96 |
| password | 128 |
| otp | 64 (normalized Base32: upper case, no spaces, `-`, `=`) |

IDs never repeat (`next id`); (site, user) is unique, case-insensitive.
Format 1 had the same bytes, sorted by name.

## fido.bin payload

LionKey's storage log (`ctap_memory_storage.c`), its used part: a sequence of
items, each

```
key (4) | size (4) | is_deleted (4) | padding (4) | data (size, padded to 4)
```

| key | Item |
|---|---|
| 1 | PIN info (`ctap_pin_persistent_state`) |
| 2 | credential (`ctap_credential_source`) |
| 3 | global signature counter (unused here: NVS `fido/sign_count`) |
| 4 | authenticatorConfig state |
| 5 | minPinLength RP ID hashes |
| 6 | U2F attestation key and certificate |
| 7 | persistent pinUvAuthTokens |
| 8 | encIdentifier device id |

The file holds only the live items: each write seals a compacted copy of the
RAM log, so a deleted credential never reaches the file or the sync server,
and loading the file rewrites nothing (no new version for synced devices to
race over). In RAM, deleted items stay flagged until the next load: the core
keeps item offsets.

## Writes and rollback

- Save: `<file>.tmp`, then remove and rename. At load, a `.tmp` without the
  file is taken (a save cut before the rename), one next to the file is
  dropped.
- NVS `vault/store_ver` and `fido/store_ver` keep the highest version
  accepted, written after the file. A file below it, or a missing file while
  a version is kept, is refused and left as is.
- PIN or seed change (both files at once): each to `<file>.new` (and
  `seed.txt.new`), then the marker file `pinchange`, then the renames, then
  the marker removed. At boot with the marker, the `.new` files are promoted;
  without it they are dropped (the old key stands).
- Sync: the server keeps the files as they are (it never has the key);
  copies are compared by `modified`, then `version`; `store id` and
  `seed id` tell which store and phrase a copy belongs to.

## seed.txt

`/spiffs/seed.txt`, plain text: the 12 BIP39 English words, lower case, one
space apart, then `\n` (`vault.cpp` `WriteSeed`, `seed_phrase.h`).

```
abandon ability able about above absent absorb abstract absurd abuse access accident
```

- 12 words = 128 bits of entropy + a 4-bit checksum (the top bits of
  SHA-256(entropy)); a phrase whose checksum does not match is ignored (no
  seed). Read forgivingly: any case and spacing, a word may be cut to its
  first 4+ letters.
- The entropy (16 bytes) is the HKDF salt of the store key; the seed id in
  the store headers is SHA-256(entropy)[0..4].
- Written through `seed.txt.tmp` and a rename; on a seed change with the
  stores, as `seed.txt.new` in the PIN-change commit (above). A new store with
  no phrase makes a random one (hardware RNG). The wipe PIN removes it.

## server.bin

`/spiffs/server.bin`, plain binary, little-endian (`server_logic.h`): the
device's identity on the authentication server.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `SRV2` |
| 4 | 1 | flags: bit 0 a key, bit 1 registered (registered needs a key; other bits 0) |
| 5 | 32 | ECDSA P-256 private key (the scalar); the public key is derived |
| 37 | 20 | TOTP secret (160 bits, RFC 4226/6238) |
| 57 | 8 | registered at, Unix seconds (int64); 0 never |
| 65 | 8 | last status the server took, Unix seconds; 0 never |
| 73 | 36 | client id (UUID `8-4-4-4-12`, NUL-padded; all zero: none) |

- Size is exactly 109 bytes. `SRV1` (73 bytes, no client id) is still read:
  registered, but signing in needs a new registration.
- Written through `server.bin.new`, then remove and rename. A new
  registration makes a new key and TOTP secret.
- The access token from the sign-in is never stored (memory only).
