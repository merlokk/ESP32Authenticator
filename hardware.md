# Hardware — XTEINK X4 Pro

Not the ESP32-C3 XTEINK X4: the X4 Pro is a different board.

## Specs

| Item | Value |
|---|---|
| MCU | ESP32-S3, 16 MB flash, 8 MB octal PSRAM |
| Radio | 2.4 GHz Wi-Fi, BLE |
| Display | 4.3" e-ink, 800×480 B/W, 219 PPI |
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

- Write-only SPI (no MISO); BUSY is active-HIGH.
- SPI clock: OEM 5 MHz, FreeInk 10 MHz, TuyaOpen 20 MHz.
- Native landscape scan. Viewable insets T/R/B/L = 9/7/3/7 px (bezel overlap).
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
| 6 | EPD BUSY | input, busy = HIGH |
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

Power-up order: GPIO1 HIGH → GPIO2 LOW (touch) → GPIO5 pulse (SD).

## I2C devices (SDA 39 / SCL 38)

| Addr | Device | Notes |
|---|---|---|
| 0x5D (alt 0x14) | GT911 touch | Self-loads its config, no upload needed. Portrait-mounted (X 0..480, Y 0..800), so swap XY. Home key is status `0x814E` bit `0x10`. |
| 0x63 | CW2017 fuel gauge | Reports 0% until the 80-byte BATINFO profile is loaded (regs `0x10`–`0x5F`). SoC is reg `0x04`, VCELL is regs `0x02`/`0x03`. |
| 0x51 | BM8563 RTC | PCF8563-compatible. |

## eFuse: flashing switches

Burned bits cannot be cleared. Some X4 Pro units ship with flashing disabled;
check with `hwinfo` / `efuse`.

| eFuse = 1 | Effect |
|---|---|
| `DIS_USB_SERIAL_JTAG` | USB Serial/JTAG controller off; cannot be re-enabled in software |
| `DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE` | no ROM flashing over USB Serial/JTAG; the app console still works |
| `DIS_DOWNLOAD_MODE` | ROM download mode off entirely (all interfaces) |

## Notes

- SD works only in native SDMMC mode; SPI-mode CMD0 gets no response.
- Frontlight PWM: 10 kHz (early OEM) or 25 kHz, 10-bit (stock 7.x).
- OEM flash layout (dual OTA, 16 MB): see [sources.md](sources.md).

## Sources

- TuyaOpen [`board_config.h`](https://github.com/tuya/TuyaOpen/blob/master/boards/ESP32/XTEINK_X4_PRO/board_config.h)
- FreeInk [`docs/xteink-x4pro-support.md`](https://github.com/Free-Ink/freeink-sdk/blob/main/docs/xteink-x4pro-support.md) (local: `../crosspoint-reader/freeink-sdk/docs/`)
- [CNX Software: X4 Pro announcement](https://www.cnx-software.com/2026/07/23/99-xteink-x4-pro-4-3-inch-touchscreen-ereader-to-support-crosspoint-reader-open-source-firmware/)
