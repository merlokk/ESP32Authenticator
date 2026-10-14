#pragma once

// What a board has and where it is wired: pure data (no driver headers), so the
// pin rules are host-tested and checked at compile time. Each board is a class
// deriving from Board (dev_board.h, x4_pro.h); devices/config.h picks one.
// GPIO numbers are int8_t, kNoPin when the line does not exist.

#include <cstddef>
#include <cstdint>

namespace boards {

inline constexpr int8_t kNoPin = -1;

enum class HwClockType : uint8_t {
    kNone,    // no battery-backed RTC: the timer clock (lost at power-off)
    kBm8563,  // PCF8563-compatible, I2C 0x51
};

enum class DisplayType : uint8_t {
    kNone,
    kEink800x480,  // X4 Pro: SSD1677 / UC8179 / UC8279, detected at runtime
};

enum class TouchType : uint8_t {
    kNone,
    kGt911,  // I2C 0x5D (alt 0x14)
};

enum class LedType : uint8_t {
    kNone,
    kWs2812,  // one addressable RGB LED
};

enum class BatteryType : uint8_t {
    kNone,
    kCw2017,  // fuel gauge, I2C 0x63
};

// Logical buttons. On the dev board the BOOT button is Left (FIDO2 user
// presence on every board).
enum class ButtonId : uint8_t { kLeft, kRight, kPower };
inline constexpr size_t kButtonCount = 3;

struct ButtonPin {
    int8_t gpio = kNoPin;
    bool active_low = true;  // pressed = 0, internal pull-up
};

struct I2cPins {
    int8_t scl = kNoPin;
    int8_t sda = kNoPin;
};

struct DisplayPins {  // write-only SPI + control lines
    int8_t mosi = kNoPin;
    int8_t sclk = kNoPin;
    int8_t cs = kNoPin;
    int8_t dc = kNoPin;
    int8_t reset = kNoPin;
    int8_t busy = kNoPin;
};

struct TouchPins {
    int8_t reset = kNoPin;
    int8_t interrupt = kNoPin;
    int8_t power_enable = kNoPin;  // active-low
};

struct SdPins {  // SDMMC 1-bit
    int8_t clk = kNoPin;
    int8_t cmd = kNoPin;
    int8_t d0 = kNoPin;
    int8_t power_enable = kNoPin;  // active-low
};

struct LedPins {
    int8_t data = kNoPin;
};

struct FrontlightPins {  // PWM, active-high
    int8_t cool = kNoPin;
    int8_t warm = kNoPin;
};

struct PowerPins {
    int8_t peripheral_rail = kNoPin;  // drive high before touch/SD
    int8_t charger_status = kNoPin;   // input, high = charging
};

class Board {
public:
    const char *name = "";

    HwClockType hw_clock = HwClockType::kNone;
    DisplayType display = DisplayType::kNone;
    TouchType touch = TouchType::kNone;
    LedType led = LedType::kNone;
    BatteryType battery = BatteryType::kNone;
    bool has_sd_card = false;
    bool has_frontlight = false;

    // FIDO2 credentials in fido.bin (the next one gets KEY_STORE_FULL). The
    // RAM log is ~1 KB per credential; the file lives on SPIFFS (80 KB, shared
    // with the stock data on the X4 Pro).
    uint16_t fido_max_credentials = 10;

    ButtonPin buttons[kButtonCount] = {};
    I2cPins i2c;
    DisplayPins display_pins;
    TouchPins touch_pins;
    SdPins sd_pins;
    LedPins led_pins;
    FrontlightPins frontlight_pins;
    PowerPins power_pins;

    constexpr const ButtonPin &Button(ButtonId id) const {
        return buttons[static_cast<size_t>(id)];
    }
    constexpr bool HasButton(ButtonId id) const { return Button(id).gpio != kNoPin; }
    constexpr bool NeedsI2c() const {
        return hw_clock != HwClockType::kNone || touch != TouchType::kNone ||
               battery != BatteryType::kNone;
    }

protected:
    constexpr Board() = default;
};

// --- Pin rules ------------------------------------------------------------------

// ESP32-S3: GPIO 0..21 and 26..48 exist. 19/20 are the native USB (console),
// 26..32 the SPI flash, 33..37 the octal PSRAM (8 MB, every board here).
constexpr bool UsableGpio(int8_t gpio) {
    if (gpio < 0 || gpio > 48 || (gpio >= 22 && gpio <= 25)) {
        return false;
    }
    return gpio != 19 && gpio != 20 && !(gpio >= 26 && gpio <= 37);
}

inline constexpr uint16_t kMaxFidoCredentials = 50;

// nullptr when the board is consistent, else what is wrong.
constexpr const char *CheckPins(const Board &b) {
    const int8_t pins[] = {
        b.buttons[0].gpio,          b.buttons[1].gpio,       b.buttons[2].gpio,
        b.i2c.scl,                  b.i2c.sda,               b.display_pins.mosi,
        b.display_pins.sclk,        b.display_pins.cs,       b.display_pins.dc,
        b.display_pins.reset,       b.display_pins.busy,     b.touch_pins.reset,
        b.touch_pins.interrupt,     b.touch_pins.power_enable, b.sd_pins.clk,
        b.sd_pins.cmd,              b.sd_pins.d0,            b.sd_pins.power_enable,
        b.led_pins.data,            b.frontlight_pins.cool,  b.frontlight_pins.warm,
        b.power_pins.peripheral_rail, b.power_pins.charger_status,
    };
    const size_t n = sizeof(pins) / sizeof(pins[0]);
    for (size_t i = 0; i < n; ++i) {
        if (pins[i] == kNoPin) {
            continue;
        }
        if (!UsableGpio(pins[i])) {
            return "a pin is not a usable GPIO (missing, USB, flash or PSRAM)";
        }
        for (size_t j = i + 1; j < n; ++j) {
            if (pins[j] == pins[i]) {
                return "two lines share a GPIO";
            }
        }
    }
    if (b.fido_max_credentials < 1 || b.fido_max_credentials > kMaxFidoCredentials) {
        return "fido_max_credentials out of 1..50";
    }
    if (b.name == nullptr || b.name[0] == '\0') {
        return "no name";
    }
    if (!b.HasButton(ButtonId::kLeft)) {
        return "no Left button (FIDO2 user presence)";
    }
    if (b.NeedsI2c() && (b.i2c.scl == kNoPin || b.i2c.sda == kNoPin)) {
        return "an I2C device without the I2C pins";
    }
    if (b.display != DisplayType::kNone &&
        (b.display_pins.mosi == kNoPin || b.display_pins.sclk == kNoPin ||
         b.display_pins.cs == kNoPin || b.display_pins.dc == kNoPin ||
         b.display_pins.reset == kNoPin || b.display_pins.busy == kNoPin)) {
        return "a display without its pins";
    }
    if (b.touch != TouchType::kNone &&
        (b.touch_pins.reset == kNoPin || b.touch_pins.interrupt == kNoPin)) {
        return "a touch panel without its pins";
    }
    if (b.has_sd_card &&
        (b.sd_pins.clk == kNoPin || b.sd_pins.cmd == kNoPin || b.sd_pins.d0 == kNoPin)) {
        return "an SD card without its pins";
    }
    if (b.led != LedType::kNone && b.led_pins.data == kNoPin) {
        return "an LED without its pin";
    }
    if (b.has_frontlight &&
        (b.frontlight_pins.cool == kNoPin || b.frontlight_pins.warm == kNoPin)) {
        return "a frontlight without its pins";
    }
    return nullptr;
}

// --- Names, for the console and the web -----------------------------------------

constexpr const char *Name(HwClockType t) {
    return t == HwClockType::kBm8563 ? "BM8563" : "none";
}
constexpr const char *Name(DisplayType t) {
    return t == DisplayType::kEink800x480 ? "e-ink 800x480" : "none";
}
constexpr const char *Name(TouchType t) { return t == TouchType::kGt911 ? "GT911" : "none"; }
constexpr const char *Name(LedType t) { return t == LedType::kWs2812 ? "WS2812" : "none"; }
constexpr const char *Name(BatteryType t) {
    return t == BatteryType::kCw2017 ? "CW2017" : "none";
}
constexpr const char *Name(ButtonId id) {
    return id == ButtonId::kLeft ? "left" : id == ButtonId::kRight ? "right" : "power";
}

}  // namespace boards
