# Credential store: encryption and size

Status: done with key B below: the store header with version and rollback
check, the RAM log in PSRAM, `vault.bin` and `fido.bin` in AES-256-GCM with
a key from the PIN and a seed phrase (salt, key id and seed id in the
headers, so synced devices with both share it; a PIN or seed change re-keys
both). Open: C (below).

## Why encrypt

Unencrypted (as it was), `fido.bin` is LionKey's store as is: per credential the P-256 private
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

B is done (PBKDF2-HMAC-SHA256 of the PIN, 6000 iterations, ~0.9 s through
PSA). With a
6-digit PIN it only holds against someone without the flash contents: from a
dump, 10^6 PINs x 12k SHA-256 each is ~10^10 hashes, seconds on a GPU. More
iterations do not fix that. The seed phrase (below) fixes it for the files
alone; C does for the device: ESP-IDF has `esp_hmac_derive_pbkdf2_key`
(PBKDF2 on the HMAC peripheral with an eFuse key that software cannot read),
so guesses must run on this chip, and the PIN lockout (10 tries) caps them.
It needs an eFuse key burn (irreversible) and, against a foreign app calling
the HMAC, secure boot or no download mode.
FIDO2 is unavailable from a reboot until the first PIN entry; off is deep
sleep (a reboot), and only `power.keepPinHours` keeps the key through it, in
RTC memory (x4-design.md). A alone
only defeats a desoldered flash chip. NVS can be encrypted by ESP-IDF with an
eFuse HMAC key (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`): fits A or C.

## Seed phrase

Key = HKDF-SHA256(salt = seed, IKM = PBKDF2(PIN, salt)); the seed is a BIP39
phrase (12 words, 128 bits) in `/spiffs/seed.txt`, the same on every device
of one store, never on the card or a sync server.

| Leaked | PIN only | PIN + seed |
|---|---|---|
| the store files (card, sync server, backup) | PIN brute-forced offline | useless (2^128) |
| the whole device (flash + card) | PIN brute-forced | the same: SPIFFS is readable without flash encryption; needs C |

The wipe PIN deletes the seed too: copies of the files elsewhere never open
on this device again.

## Cipher and file format

- AES block is always 16 B; 192/256 is the key length. AES-192 gains nothing
  and the S3 accelerator does 128 and 256 only. Use **AES-256-GCM**: CTR-based,
  any length, no padding (no 16-byte boundary); 16-byte tag = integrity (a
  tampered or truncated file is refused). CBC (in `crypto` now) would need
  PKCS#7 padding and a separate HMAC.
- File: the store header (`store_header.h`, 80 bytes, format 3: kind,
  cipher, version, length, modification time, nonce, PBKDF2 iterations and
  salt, key id, store id, seed id) | ciphertext | tag (16). The header is
  the GCM associated data. A random nonce per write: never reused with the
  same key.
- Rollback to an older valid file is not detected by GCM: the header's
  version is checked against the highest one kept in NVS (both stores). Sync: the later modification time wins (x4-design.md).

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

- **Store in PSRAM** (8 MB on the dev board and the X4 Pro; done): the RAM
  log is sized from the board's `fido_max_credentials` (1 KB each + 2 KB),
  e.g. 512 KB for ~500 credentials on SD. Linear scans over a few hundred KB
  of PSRAM per request: well under a ms. The natural fit with encryption.
- **Reading items from flash on demand** does not fit: SPIFFS and SD cannot be
  mapped, so every item would need a decrypted cache pinned for as long as
  LionKey holds the handle, i.e. the whole store anyway. The one case that
  maps: a raw flash partition via `esp_partition_mmap` with hardware flash
  encryption (transparent decrypt on read); it needs flash encryption on, not
  possible for SD.
- **Writes on SD** (decided): the whole file per change, see "Size and
  writes".
- **Which store**: one LionKey instance has one storage. Simplest: pick at boot
  (SD present: the SD file, else SPIFFS). Both at once needs a composite backend
  that tags handles with their store.
- LionKey limits at scale: `max_credentials` is ours (count check in
  `ctap_file_storage.c`); credential management enumerates up to 128 RPs and
  128 credentials per RP; an assertion lists up to 128 credentials per RP.

## Size and writes

Target: ~500 FIDO2 credentials and ~500 password records.

| | Typical | Worst |
|---|---|---|
| 500 FIDO2 credentials (measured ~200 B each on webauthn.io; up to ~635 B + 64 B origin) | 150-200 KB | 350 KB |
| 500 records (site, login, password, TOTP secret; fixed 512 B or variable) | ~100 KB | 256 KB |
| In PSRAM, with the 1 KB/credential reservation and the boot compaction buffer | | ~1.3 MB of 8 MB |

PSRAM on the X4 Pro (125 credentials, 500 records): the FIDO2 log 127 KB,
the records ~1.2 KB each (~600 KB), LVGL's buffers. Internal RAM holds none
of it.

Where: microSD (X4 Pro: 125 credentials, 500 records). SPIFFS is 80 KB
(68 KB usable, stock X4 Pro table): it keeps the stores for boards without
a card slot (dev board: 20 credentials, 50 records).

How (decided): each store is one encrypted file, loaded whole into PSRAM at
unlock and rewritten whole (sorted, if wanted: it is all in RAM) on a change.

- Two files, same header, different `kind`: the FIDO2 log (LionKey format) and
  the records. A password change does not rewrite the FIDO2 file. One key.
- One AES-256-GCM pass per file, no per-record crypto.
- Cost per change: 250-400 KB over SPI (~0.2-0.4 s) plus GCM (HW AES, GHASH in
  software: tens of ms). Rare (register, delete, edit), so acceptable. SD wear
  is the card controller's job.
- Atomic replace as on SPIFFS: FAT cannot rename over a file either, so the
  same `.tmp`, remove, rename and the same recovery at boot.
- Card removed: the stores stay in PSRAM; changes are refused (or kept as
  unsaved until the card is back).

Not pursued: changing one record in place. SPIFFS (`spiffs_nucleus.c`)
copies on write per 256-byte page (251 data bytes) and rewrites the index
page, erasing only in GC, so an in-place record costs ~3-4 pages against ~125
for a 30 KB rewrite (~30-40x less wear). It would need per-record GCM (random
nonce, record ID and version as associated data) and a header MAC over all
record tags against single-record rollback. It only pays on SPIFFS, which is
too small for these stores anyway.

## NVS writes

Counters live in NVS: PIN failures and lock deadline, the store's rollback
counter, the FIDO2 signature counter. NVS appends 32-byte entries and erases a
4 KB page only when it recycles it; the partition is 5 pages (630 entries, ~380
free today), so a page is erased about once per few hundred writes. The flash
is NOR (X4 Pro: Puya, JEDEC 0x852018; 100k erase cycles typical): tens of millions of writes.
Rules that keep it so:

- Write a value only when it changes, never per tick or per check.
- Nothing that changes all the time (clocks, "last seen") goes into NVS.
- Keep free entries: the NVS partition cannot grow (stock X4 Pro table), so
  bulk data (the store, `fido.bin`) is files; NVS holds counters only.

## Plan

1. GCM container between RAM and file, LionKey untouched (done:
   `storefile/store_io`).
2. SD backend (RAM log in PSRAM and `max_credentials` per board: done).
3. Key source per the threat model (lost / stolen device): B with the seed
   phrase done; C open, any eFuse burn only with explicit consent
   (irreversible).
4. Same container for `config.json`; NVS encryption with the eFuse HMAC key.
