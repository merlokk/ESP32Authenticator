# Hardware — XTEINK X4 Pro

Not the ESP32-C3 XTEINK X4: the X4 Pro is a different board.

## Specs

| Item | Value |
|---|---|
| MCU | ESP32-S3 (QFN56) rev v0.2, 40 MHz XTAL |
| Flash | 16 MB, quad: Puya (JEDEC 0x85/0x2018) on our open unit, XTX (0x0b/0x4018) on the locked one; stock image header DIO 80 MHz |
| PSRAM | 8 MB embedded octal, AP 3.3 V |
| Radio | 2.4 GHz Wi-Fi, BLE |
| Display | 4.3" e-ink, 800×480 B/W (4 grey levels on the UC8179: `epd::RefreshGrey`), 219 PPI |
| Touch | GT911 capacitive + capacitive Home key |
| Frontlight | dual warm/cool PWM |
| Input | Left, Right, Power buttons + Home (touch) |
| Storage | microSD (SDMMC 1-bit) |
| RTC | BM8563 (PCF8563-compatible) |
| Battery | 1100 mAh, CW2017 fuel gauge, pogo-pin charging |
| USB | ESP32-S3 native USB |
| Size | 111×69×5.95 mm, 72 g |

## Display controllers

Varies by production batch. Same glass and pinout for all of them; detect the controller at runtime (see FreeInk `applyXteinkDisplayController()`).

| Controller | Notes |
|---|---|
| SSD1677 | Original units. Internal booster, OTP waveform; FULL `0x22=0xF7`, FAST `0x22=0xFC`. |
| UC8179 | UltraChip variant. |
| UC8279 | UltraChip variant. |

Stock firmware 7.5.11 ships waveforms for UC8179 and UC8279 (`*_gray_full`,
`*_gray_aa`, `*_aa_prebw_mid`) and an `ESP32S3_X4_TL_SSD1677` board name; the
panel kind is stored in NVS `hw_calib` (`screenType`, `screenId`; both 1 on our
unit, meaning unknown).

- Write-only SPI (no MISO); BUSY: SSD1677 high while busy, UC81xx low
  (BUSY_N).
- SPI clock: OEM 5 MHz, FreeInk 10 MHz, TuyaOpen 20 MHz.
- Native landscape scan. Viewable insets T/R/B/L = 9/7/3/7 px (bezel overlap;
  FreeInk). Our unit, measured in 4 px steps, in frame coordinates
  (landscape): 4/2/4/8. Held as a book (portrait, the default) the frame's
  left edge (x 0) is on top and its top edge (y 0) on the right: the UI turns
  it 90 degrees.
- Our unit: UC8179, FLG 0x13, VER `00 00 01 FF FF` (= stock screenType 1).
  Full refresh 1.9 s, fast 0.9 s, each with the plane uploads and power on/off.
- The data line reads back: a UC81xx answers FLG (0x71) and VER (0x70) on
  GPIO11 as input (how the controller is detected).
- No external EPD PMIC or GPIO power enable.

## Pinout

| GPIO | Function | Notes |
|---|---|---|
| 0 | Button Left | active-LOW, pull-up; boot-strap pin |
| 1 | Peripheral rail | drive HIGH first; required for touch |
| 2 | Touch power enable | active-LOW |
| 3 | Button Power | active-LOW, pull-up |
| 4 | GT911 RST | |
| 5 | SD power enable | active-LOW; pulse HIGH→LOW before mount, keep LOW |
| 6 | EPD BUSY | input; busy = HIGH on the SSD1677, LOW on the UC81xx |
| 7 | Button Right | active-LOW, pull-up |
| 8 | Frontlight cool | PWM, active-HIGH |
| 9 | Frontlight warm | PWM, active-HIGH |
| 10 | GT911 INT | |
| 11 | EPD MOSI | |
| 12 | EPD SCLK | |
| 13 | EPD CS | |
| 14 | EPD RST | |
| 18 | EPD DC | |
| 19 | USB D− | do not repurpose or probe |
| 20 | USB D+ | do not repurpose or probe |
| 21 | Charger STAT | input, no pull; HIGH = charging |
| 38 | I2C SCL | 400 kHz |
| 39 | I2C SDA | |
| 40 | SD DAT0 | |
| 41 | SD CLK | 40 MHz |
| 42 | SD CMD | |

Buttons on the X4 Pro (measured): idle high, pressed low with the internal pull-up; a 1.1 s hold of Power does not cut the power.

Power-up order: GPIO1 HIGH → GPIO2 LOW (touch) → GPIO5 pulse (SD).

## I2C devices (SDA 39 / SCL 38)

| Addr | Device | Notes |
|---|---|---|
| 0x5D (alt 0x14) | GT911 touch | Self-loads its config, no upload needed. Portrait-mounted (X 0..480, Y 0..800): on the 800x480 landscape frame swap XY; on the 480x800 portrait UI it maps as is. Home key is status `0x814E` bit `0x10`. Our unit: firmware 0x1060, vendor 0x00, config version 0x41 (`A`), output 480 x 800, 5 contacts, INT falling. Contact records: x, y, size (LE) from byte 0, byte 6 zero, the track id in byte 7 (two fingers: 1 and 0, kept while they move); with the device held buttons down, top left read x 38 y 16, bottom right x 466 y 794. A still Home key makes no frames: its state is latched. |
| 0x63 | CW2017 fuel gauge | Reports 0% until the 80-byte BATINFO profile is loaded (regs `0x10`–`0x5F`, flag `0x0B` bit 7); it keeps it while the cell is connected (stock loads the same table). SoC is reg `0x04` (+ `0x05`/256), VCELL is regs `0x02`/`0x03` (14 bits, 312.5 uV). VERSION `0x0F` on our unit. No current register: charging only from GPIO21. |
| 0x51 | BM8563 RTC | PCF8563-compatible. Stock keeps UTC in it. After STOP clears, the first tick comes 1.000 s later (PCF8563: 0.5078 s). |

## eFuse: flashing switches

Burned bits cannot be cleared. Some X4 Pro units ship with flashing disabled;
check with `hwinfo` / `efuse`; installing on one:
[hardware-locked.md](hardware-locked.md). Our unit (2026-10): all of these 0, secure boot
and flash encryption off, key blocks empty; BLOCK3 (user data) holds the
factory serial (`X4CB02EN…`). Our locked unit: `DIS_USB_SERIAL_JTAG`,
`DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE`, `DIS_USB_OTG_DOWNLOAD_MODE` burned, the
rest 0 ([hardware-locked.md](hardware-locked.md#our-locked-unit-2026-10-29)).

| eFuse = 1 | Effect |
|---|---|
| `DIS_USB_SERIAL_JTAG` | USB Serial/JTAG controller off; cannot be re-enabled in software |
| `DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE` | no ROM flashing over USB Serial/JTAG; the app console still works |
| `DIS_USB_OTG_DOWNLOAD_MODE` | no ROM flashing over USB-OTG |
| `DIS_DOWNLOAD_MODE` | ROM download mode off entirely (all interfaces) |

## Notes

- SD works only in native SDMMC mode; SPI-mode CMD0 gets no response. The
  stock card (ASTC 16 GB, FAT: `Books/`, `Fonts/`, `XTData/`, `XTCache/`)
  mounts at 40 MHz on the first attempt.
- Frontlight PWM: 10 kHz (early OEM) or 25 kHz, 10-bit (stock 7.x). Checked on
  our unit at 25 kHz: steady, GPIO8 cool, GPIO9 warm, 1 LSB of 1023 still visible.
- Stock firmware (`xteink_app` 7.5.11, ESP-IDF v6.0.1): silent on the USB
  console, exposes the microSD as USB MSC, talks to `api-prod.xteink.cc|cn`.
  Full backup and details: [originalfw/README.md](../originalfw/README.md).
- Stock partition table:

| Name | Type/SubType | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20K |
| otadata | data/ota | 0xe000 | 8K |
| app0 | app/ota_0 | 0x10000 | 8064K |
| app1 | app/ota_1 | 0x7f0000 | 8064K |
| spiffs | data/spiffs | 0xfd0000 | 80K |
| coredump | data/coredump | 0xfe4000 | 112K |

## Sources

- TuyaOpen [`board_config.h`](https://github.com/tuya/TuyaOpen/blob/master/boards/ESP32/XTEINK_X4_PRO/board_config.h)
- FreeInk [`docs/xteink-x4pro-support.md`](https://github.com/Free-Ink/freeink-sdk/blob/main/docs/xteink-x4pro-support.md) (local: `../crosspoint-reader/freeink-sdk/docs/`)
- [CNX Software: X4 Pro announcement](https://www.cnx-software.com/2026/07/23/99-xteink-x4-pro-4-3-inch-touchscreen-ereader-to-support-crosspoint-reader-open-source-firmware/)
