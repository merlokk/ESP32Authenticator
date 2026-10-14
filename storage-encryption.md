# Credential store: encryption and size

Status: analysis, nothing implemented. Open decision: the key source (below).

## Why encrypt

`/spiffs/fido.bin` is LionKey's store as is: per credential the P-256 private
key (32 B), hmac-secret randoms, RP ID, user name; plus the PIN state
(LEFT(SHA-256(PIN), 16) and the retry counter).

The dev board has flash encryption and secure boot off, USB download mode on
and all six eFuse key blocks free (`hwinfo`). `esptool read-flash` over USB
copies the file in a minute:

- every key is cloned: a copy of the authenticator works on every site;
- the PIN is brute-forced offline (4-6 digits: instantly); the retry counter
  does not apply.

Same exposure: `config.json` (Wi-Fi and web passwords), NVS (BLE bond keys).

## The key is the question, not the cipher

A key stored in flash next to the file adds nothing. Options:

| | Key | Protects against | Cost |
|---|---|---|---|
| A | eFuse key block, purpose HMAC_UP (unreadable by software); KEK = HMAC(eFuse, "fido.bin") | a flash dump; the file is bound to the chip | eFuse burn is irreversible. With download mode open, an attacker flashes an app that calls HMAC itself: full protection needs secure boot / no download mode, which conflicts with the X4 Pro stock OTA path |
| B | derived from a master password (PBKDF2 on the HW SHA, ~1 s) | flash dump and foreign firmware: no key on the device | FIDO2 works only after unlock (password entry: buttons + e-ink, console or web) |
| C | KDF(HMAC eFuse, password) | as B, and a copied file does not open on another chip | both |

Target: B or C, one PIN-derived key for the records store and `fido.bin`.
FIDO2 is unavailable from a reboot until the first PIN entry; a locked screen
keeps the key in RAM, so FIDO2 keeps working (x4-design.md). A alone
only defeats a desoldered flash chip. NVS can be encrypted by ESP-IDF with an
eFuse HMAC key (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`): fits A or C.

## Cipher and file format

- AES block is always 16 B; 192/256 is the key length. AES-192 gains nothing
  and the S3 accelerator does 128 and 256 only. Use **AES-256-GCM**: CTR-based,
  any length, no padding (no 16-byte boundary); 16-byte tag = integrity (a
  tampered or truncated file is refused). CBC (in `crypto` now) would need
  PKCS#7 padding and a separate HMAC.
- File: `magic+version (4) | nonce (12, random per write) | ciphertext | tag (16)`:
  +32 B. Never reuse a nonce with the same key.
- Rollback to an older valid file is not detected by GCM: the header carries a
  counter and a modification time (authenticated as GCM associated data), the
  highest accepted counter is kept in NVS, and a file below it is invalid.
  Sync: the later modification time wins (x4-design.md).

## Speed

- Decrypt once at boot into RAM; LionKey keeps working on the RAM copy, CTAP
  requests are unaffected. The RAM copy is plaintext (unavoidable).
- Written only on register / delete / PIN change (the signature counter is in
  NVS: assertions write nothing).
- GCM over 12 KB: ~ms (HW AES for CTR, GHASH in software); the SPIFFS write of
  the same data: tens to hundreds of ms. No slowdown worth measuring.

## Two stores: SPIFFS (key-only devices) and microSD (hundreds of keys)

LionKey hands out pointers into the store (`ctap_storage_item_t.data`,
`ctap_credential_handle_t.source`) and keeps them across requests
(`getNextAssertion`, credential management enumerations, up to 128 handles).
So the decrypted store must sit in memory at stable addresses:

- **Store in PSRAM** (8 MB on the dev board and the X4 Pro): the RAM log moves
  from the 12 KB static in internal RAM to a PSRAM buffer sized per backend:
  12 KB / 10 credentials for SPIFFS, e.g. 512 KB / ~500 credentials for SD (a
  credential is 200-640 B). Linear scans over a few hundred KB of PSRAM per
  request: ~ms. This is the natural fit with encryption.
- **Reading items from flash on demand** does not fit: SPIFFS and SD cannot be
  mapped, so every item would need a decrypted cache pinned for as long as
  LionKey holds the handle, i.e. the whole store anyway. The one case that
  maps: a raw flash partition via `esp_partition_mmap` with hardware flash
  encryption (transparent decrypt on read); it needs flash encryption on, not
  possible for SD.
- **Writes on SD**: rewriting a ~300 KB file per change costs ~100+ ms plus GCM;
  acceptable (rare). Better later: an append-only encrypted log on disk, one
  GCM-sealed record per change, compacted at boot (the same shape as LionKey's
  RAM log), so a change writes one record.
- **Which store**: one LionKey instance has one storage. Simplest: pick at boot
  (SD present: the SD file, else SPIFFS). Both at once needs a composite backend
  that tags handles with their store.
- LionKey limits at scale: `max_credentials` is ours (count check in
  `ctap_file_storage.c`); credential management enumerates up to 128 RPs and
  128 credentials per RP; an assertion lists up to 128 credentials per RP.

## NVS writes

Counters live in NVS: PIN failures and lock deadline, the store's rollback
counter, the FIDO2 signature counter. NVS appends 32-byte entries and erases a
4 KB page only when it recycles it; the partition is 5 pages (630 entries, ~380
free today), so a page is erased about once per few hundred writes. The flash
is a Macronix NOR (100k erase cycles typical): tens of millions of writes.
Rules that keep it so:

- Write a value only when it changes, never per tick or per check.
- Nothing that changes all the time (clocks, "last seen") goes into NVS.
- Keep free entries: the NVS partition cannot grow (stock X4 Pro table), so
  bulk data (the store, `fido.bin`) is files; NVS holds counters only.

## Plan

1. GCM container in `ctap_file_storage.c` (between RAM and file; LionKey
   untouched), key through one `StorageKey()` so the source can change.
2. RAM log in PSRAM, size and `max_credentials` per backend; SD backend.
3. Key source per the threat model (lost / stolen device: B or C); any eFuse
   burn only with explicit consent (irreversible).
4. Same container for `config.json`; NVS encryption with the eFuse HMAC key.
