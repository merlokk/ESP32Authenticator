# X4 Pro: functions and screen design

Target design for the XTEINK X4 Pro (e-ink 800x480, 1-bit, GT911 touch, Left /
Right / Power buttons, frontlight, microSD). Status per function:
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
| Encrypted store | one `.bin` file: records + settings, AES-256-GCM, with a data version |
| Sync | over the internet; service not chosen yet (may be our own). The newer data version replaces the older one |
| Settings | frontlight, Wi-Fi; more later |

## Screen

```
+--------------------------------------------------------------+
| Wi-Fi  BT  battery  SD  storage(sync)              12:34     |  status bar
+--------------------------------------------------------------+
| github.com      user@mail                                    |
| mail.google.com user@gmail.com                               |  credential list
| ...                                                          |  (one row per record)
+--------------------------------------------------------------+
|   <       [1]  2   3   4       >                             |  pages
+--------------------------------------------------------------+
```

- **Status bar**: Wi-Fi, Bluetooth (host connected), battery, SD card,
  connection to the sync storage.
- **List**: records as in a password manager. A tap on a row types into the
  host: left third the login, centre the password, right third the current
  TOTP code.
- **Pages**: pages of the credential list; switcher at the bottom, 4 pages
  for now, the Left / Right buttons flip them as well.
- **Lock screen**: PIN pad (touch), shown at boot and after an idle timeout.
- **Settings**: frontlight, Wi-Fi.

E-ink: 1-bit LVGL, redraw at most every 200 ms; partial updates for list and
status changes, a full refresh now and then against ghosting (driver's choice).

## Store file and sync

- One encrypted `.bin` (records and settings): header with format version and
  **data version**, then AES-256-GCM (format: storage-encryption.md).
- The data version grows with every change. Sync: whichever copy has the
  higher version wins and overwrites the other.

## Open questions

- Sync: the service; edits on two copies from the same version (the higher
  version silently wins); detecting a rolled-back file (stored version in NVS).
- PIN: length, attempts before wipe / delay; whether FIDO2 also waits for unlock.
- Panel orientation (landscape 800x480 or portrait 480x800).
