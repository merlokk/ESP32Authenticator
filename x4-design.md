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

Portrait, 480x800. The panel scans landscape (hardware.md): the driver rotates.

```
+------------------------------+
| WiFi BT bat SD sync   12:34  |  status bar
+------------------------------+
| github.com                   |
|   user@mail                  |
| mail.google.com              |  credential list
|   user@gmail.com             |  (one row per record)
| ...                          |
|                              |
+------------------------------+
|  <   [1]  2   3   4   >      |  pages of the list
+------------------------------+
```

- **Status bar**: Wi-Fi, Bluetooth (host connected), battery, SD card,
  connection to the sync storage.
- **List**: records as in a password manager. A tap on a row types into the
  host: left third the login, centre the password, right third the current
  TOTP code.
- **Pages**: pages of the credential list; switcher at the bottom, 4 pages
  for now, the Left / Right buttons flip them as well.
- **Lock screen**: PIN pad (touch), shown at boot and after an idle timeout.
  See PIN below.
- **Settings**: frontlight, Wi-Fi.

E-ink: 1-bit LVGL, redraw at most every 200 ms; partial updates for list and
status changes, a full refresh now and then against ghosting (driver's choice).

## PIN

- 6 digits. Unlocks the screen and derives the storage key.
- 10 wrong PINs in a row: locked for `lockMinutes` (config); then 10 more
  tries. The failure count and the lock deadline live in NVS, so a reboot
  does not reset them.
- FIDO2 does not wait for the unlock: it works on a locked device (user
  presence: the button). So `fido.bin` cannot be under the PIN-derived key;
  it needs a device key (option A in storage-encryption.md) or stays plain.
- A special **wipe PIN** (config) erases the store and the FIDO2 credentials
  (as `fido2 reset`) when typed on the PIN pad.
  It has to be checked before anything is decrypted, so config keeps only a
  salted hash of it, never the PIN; it must differ from the real PIN.

## Store file and sync

- One encrypted `.bin` (records and settings). Header: format version,
  **counter**, **modification time** (UTC), then AES-256-GCM (format:
  storage-encryption.md). The header is authenticated (GCM associated data):
  counter and time cannot be edited without the key.
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
