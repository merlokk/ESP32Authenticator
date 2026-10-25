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
| Settings | frontlight, Wi-Fi, Bluetooth, display & power, security, date & time, about |

## Screen

Portrait, 480x800. The panel scans landscape (hardware.md): `ui_logic.h` turns the LVGL screen onto it.

```
+------------------------------+
| WiFi BT SD        12:34 bat  |  status bar
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
  battery; later the sync storage. While FIDO2 waits: Left or Home yes, Right no; a window
  over everything says what is asked (create a key / sign in / U2F / reset /
  selection), the site, the account (a sign-in: the store's key in the allowList,
  else for the site; of several the first 4 names and "+N more"), whether the PIN was given, with "< Allow" and "Reject >" by
  the buttons (the edges, at the password list's second row) and "Home:
  allow" at the bottom (`fido_prompt_logic.h` reads the request).
- **List**: the records in the user's order, set on the web site (up /
  down, to the top / bottom per record, "Sort A to Z") or the console (`pass move`, `pass sort`);
  a new one goes last. The password hidden, the TOTP code shown (new every
  30 s). A row's quarters: type the user, type the password, show the
  password (20 s), type the TOTP code. While the password is shown, a tap
  anywhere in the row only hides it (nothing typed).
- **Pages**: a tab per page at the bottom, 4 at most (the row scrolls);
  Left / Right flip them, Home: page 1.
- **Settings**: a swipe down from the top quarter; its back arrow, a swipe
  up or Home closes it. As on a phone: the frontlight on top (a switch,
  Level and Warmth sliders: lit at once, saved when let go), then a row per
  page with its summary and ">": Wi-Fi, Bluetooth, Web site,
  Devices, Security, Date & time, About. A page lists "label  value" lines (looked at every
  2 s) and may have one button: Power off, Lock now, Sync the time now; its
  arrow goes back to Settings. Wi-Fi is a window of its own: a switch, the
  state, Mode and access point (the mode: client / access point / both; the
  access point's name, password, channel, after how many rounds it opens
  and how long it stays; the hostname), the saved networks (a tap: Connect,
  i.e. tried first; IP address: DHCP or static, the fields on a digit
  keyboard, checked whole on Save; Forget), "Add a network": a scan
  (strongest first, padlock, signal bars, "saved"), a tap joins a saved or
  open one or asks the password on the keyboard (OK joins, saved to
  config.json); "Other...": a hidden network, its name then its password. Bluetooth too: a switch (now and at
  boot), the state, the name (a tap: change it on the keyboard, or the
  board's; applied at once, `ble.name`), the paired hosts (a tap: Use only this host /
  Let any paired host in, Forget), "Pair a new host": a 60 s window, what to
  do on the host, the passkey in big digits. Devices > Touchscreen: five crosses one by
  one, a straight line per axis through the raw taps (scale and offset,
  `display.touch` in config.json), Save / Again / Use the default; a fit far
  off (a tap beside a cross) is refused. Devices: the battery, then
  Light off after and On USB, light off after (never .. 30 min; on again at
  the next touch or button), Power off after and On USB, off after (never ..
  1 hour; USB power: a host, or the charger charging, as a charger with a
  full cell is not seen), PIN after sleep (always .. a week),
  Wallpaper (grey / black & white), Power in sleep (kept / off:
  `power.holdRail`, off loses the kept PIN): a tap lists the choices with a check at
  the one in use, a tap keeps one (config.json); Touchscreen (its calibration);
  what the Power button does;
  Power off. Date & time: the clock big with the date and the offset; Time
  zone (a region, then its cities A to Z, 9 a page with < 1 / 3 >), Sync (never ..
  every week), the server and the last sync, Set the time by hand (year ..
  minute with - / +, local time, Save sets the board's clock too), Sync the
  time now. Security: the vault and FIDO2 (records, keys, its PIN), Change the
  PIN (on a pad of its own: the current PIN, a new one twice; vault.bin and
  fido.bin re-keyed), 10 wrong PINs: lock (1 min .. a day), PIN complexity
  (the vault at once, FIDO2 from the next start), Wipe PIN (set twice /
  change / off), Lock now; its FIDO2 row opens FIDO2 / U2F: running or why
  not, the keys (of how many, resident), its PIN and tries, the shortest PIN,
  complexity, always verify, the store, the signatures, how presence is
  given; Show the keys (the list), Reset FIDO2 (held 4 s: every key and its
  PIN erased). About: the name and the model big, then
  firmware, build, ESP-IDF, LVGL, chip, flash and PSRAM, the MACs, the web
  site, uptime, free memory, SD card (a snapshot), Restart (asked first: the
  PIN after it). Web site: running or why not, the address and its QR code,
  Runs (off / on / on the device's access point), Changes from the site
  (web.write), Login (set on the keyboard, the user then the password; or
  off).
- **U2F / FIDO2 list**: a swipe up from the bottom quarter; the keys by site
  (A to Z), 6 a page: site, user, "made <date>, <alg>, resident / U2F";
  Left / Right or the bar under it (‹, "1 / 2", ›) flip the pages, the title says the count; its
  back arrow, a swipe down or Home closes it. Delete: a swipe left on a key
  shows a trash can at its right end (a swipe right or a tap elsewhere hides
  it); the can asks "Delete this key?": No (or Right) at once, Yes held 4 s
  (it fills from the left; let go early: empty again).
- **Dialogs** (`dialog.h`), one at a time over the page: a choice (a title,
  a button per answer; Right: its last, Cancel) or a question with a held Yes
  (No at once, Yes held 4 s, filling from the left; Right: No).
- **Lock screen**: PIN pad (touch), shown while the vault is locked (at boot,
  `pin lock`); done (`pin_pad.cpp`).
  See PIN below.

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
