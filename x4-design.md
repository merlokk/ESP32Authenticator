# X4 Pro: functions and screen design

Target design for the XTEINK X4 Pro (e-ink 480x800 portrait, 1-bit, GT911
touch, Left / Right / Power buttons, frontlight, microSD). Status per function:
[architecture.md](architecture.md#functionality).

## Today

The dev board is a FIDO2 key: CTAP 2.1 over BLE (done) and USB (not yet), plus
the console, the configuration site, the BLE keyboard and `totp` on the console.

## Functions

| Function | |
|---|---|
| FIDO2 key | CTAP 2.1 over BLE and USB; user presence: a button (later: a tap) |
| PIN lock | the screen is locked by a PIN; the same PIN derives the storage key (option B / C in [storage-encryption.md](storage-encryption.md)), so nothing opens without it |
| Password manager | records: site (name), login, password, TOTP secret; typed into the host over the BLE keyboard |
| Encrypted store | one `.bin` file: records + settings, AES-256-GCM, with a counter and a modification time |
| Sync | over the internet; service not chosen yet (may be our own). The copy changed last wins (see below) |
| Settings | frontlight, Wi-Fi; more later |

## Screen

Portrait, 480x800. The panel scans landscape (hardware.md): `ui_logic.h` turns the LVGL screen onto it.

```
+------------------------------+
| WiFi BT SD        12:34 bat  |  status bar (FIDO2: <- yes -> no)
|  user | pass | show | TOTP   |  what a tap in each quarter does
+------------------------------+
| github.com           123 456 |  a record: tap a quarter to type the
| user@mail          ********  |  user / password, show it, type TOTP
| ...  (6 records a page)      |
+------------------------------+
| [1]   2    3    4 >          |  a tab per page, 4 shown
+------------------------------+
```

- **Status bar**: Wi-Fi, Bluetooth (host connected), SD card, time,
  battery; later the sync storage. While FIDO2 waits: Left yes, Right no.
- **List**: the records in the user's order, set on the web site (up /
  down per record, "Sort A to Z") or the console (`pass move`, `pass sort`);
  a new one goes last. The password hidden, the TOTP code shown (new every
  30 s). A row's quarters: type the user, type the password, show the
  password (20 s), type the TOTP code.
- **Pages**: a tab per page at the bottom, 4 at most (the row scrolls);
  Left / Right flip them, Home: page 1.
- **Settings**: a swipe down from the top quarter; its back arrow, a swipe
  up or Home closes it.
- **U2F / FIDO2 list**: a swipe up from the bottom quarter; the keys by site
  (A to Z), 6 a page: site, user, "made <date>, <alg>, resident / U2F";
  Left / Right or the bar under it (‹, "1 / 2", ›) flip the pages, the title says the count; its
  back arrow, a swipe down or Home closes it.
- **Lock screen**: PIN pad (touch), shown while the vault is locked (at boot,
  `pin lock`); done (`pin_pad.cpp`).
  See PIN below.
- Settings (frontlight, Wi-Fi) is a stub for now.

E-ink: 1-bit LVGL, redraw at most every 200 ms; partial updates for list and
status changes, a full refresh now and then against ghosting (driver's choice).

## PIN

- 6 digits. Unlocks the screen and derives the storage key.
- 10 wrong PINs in a row: locked for `lockMinutes` (config); then 10 more
  tries. The failure count and the lock deadline live in NVS, so a reboot
  does not reset them.
- One key from the PIN for the store and `fido.bin` (salt and key id in the
  headers: synced devices with the PIN derive the same key; a PIN change
  makes a new key and rewrites both). After a reboot nothing
  is decrypted and FIDO2 does not work until the first PIN entry.
- Locking the screen (idle, sleep) keeps the key and the decrypted data in
  RAM: FIDO2 keeps working on a locked device (user presence: the button).
  Light sleep keeps RAM; deep sleep or power-off is a reboot (PIN again,
  unless `power.keepPinHours` kept the key in RTC memory; Power x3: locked).
  Power-off (Power held, idle `power.idleMinutes`) shows a random wallpaper
  from `/sdcard/wallpapers`.
- A special **wipe PIN** (config) erases the store and the FIDO2 credentials
  (as `fido2 reset`) and the key in RAM when typed on the PIN pad.
  It has to be checked before anything is decrypted, so config keeps only a
  salted hash of it, never the PIN; it must differ from the real PIN.
  Done on the console (`pin`, `pin wipe set`): the store file and its kept
  version go, so the next PIN makes a new, empty store.

## Store file and sync

- One encrypted `.bin` (records and settings). Header (`store_header.h`, as
  on `fido.bin` now): format, **counter** (version), **modification time**
  (UTC), then AES-256-GCM (format: storage-encryption.md). The header is
  authenticated (GCM associated data): counter and time cannot be edited
  without the key.
- Every change: counter + 1, modification time = now. The X4 Pro always has
  a time (the BM8563 RTC, corrected by SNTP), maybe off by some drift. A board
  with no believable time (the dev board before its first sync) still saves
  the edit, with time 0 (unknown).
- **Sync**: the copy with the later modification time wins, also when both
  copies were changed (on two devices) since the last sync; equal times: the
  higher counter. Time 0 loses to any known time. The result is written with
  counter = max(both) + 1, so it is above what either device has seen.
- **Rollback**: the device keeps the highest counter it has accepted in NVS. A
  `.bin` with a lower counter is invalid (an old copy put back on the SD card
  or served by the sync service).

## Open questions

- Sync service.
- Sleep: light (RAM and key kept, FIDO2 without a PIN) or deep (a reboot: PIN
  again), decided by measured power draw.
