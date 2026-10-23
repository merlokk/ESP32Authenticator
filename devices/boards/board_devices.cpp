#include "board_devices.h"

#include <cstdio>

#include <cinttypes>

#include "bm8563_clock.h"
#include "clock_math.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "i2c_bus.h"
#include "led.h"
#include "timer_clock.h"

namespace boards {

namespace {

constexpr const char *TAG = "board";

// UART1: UART0 is the ROM's and the default console's.
constexpr uart_port_t kLedUart = UART_NUM_1;

constexpr uint32_t kTypingLedMs = 1000;
// Longer than the CTAP button wait (30 s): the end of the wait ends it.
constexpr uint32_t kUserWaitLedMs = 35000;

i2cbus::Bus i2c_bus;
rtc::TimerClock timer_clock;
rtc::Bm8563Clock bm8563_clock(i2c_bus);
rtc::Clock *active_clock = &timer_clock;

void PrintPin(const char *label, int8_t gpio) {
    if (gpio != kNoPin) {
        printf(" %s=%d", label, gpio);
    }
}

void PrintUtc(const char *label, int64_t utc_us) {
    const rtc::DateTime t = rtc::FromUnix(utc_us / 1000000);
    printf("%-10s %04u-%02u-%02u %02u:%02u:%02u.%03u UTC\n", label, t.year, t.month, t.day, t.hour,
           t.minute, t.second, static_cast<unsigned>(utc_us % 1000000 / 1000));
}

}  // namespace

esp_err_t Init() {
    ESP_LOGI(TAG, "%s", kBoard.name);
    if (kBoard.led == LedType::kWs2812 &&
        led::Init(kLedUart, static_cast<gpio_num_t>(kBoard.led_pins.data)) == ESP_OK) {
        // Red solid while booting: a dark board is a hardware fault, a red one
        // is running firmware, and it shows a reset from across the room.
        led::Set(led::colour::kRed, led::Effect::kSolid);
    }
    if (kBoard.power_pins.peripheral_rail != kNoPin) {
        // First: touch and SD sit behind it (hardware.md, power-up order).
        const auto rail = static_cast<gpio_num_t>(kBoard.power_pins.peripheral_rail);
        gpio_reset_pin(rail);
        gpio_set_direction(rail, GPIO_MODE_OUTPUT);
        gpio_set_level(rail, 1);
    }
    if (kBoard.NeedsI2c()) {
        // Not fatal: its devices report it.
        i2c_bus.Init(static_cast<gpio_num_t>(kBoard.i2c.scl),
                     static_cast<gpio_num_t>(kBoard.i2c.sda));
    }
    if (kBoard.hw_clock == HwClockType::kBm8563) {
        const esp_err_t rtc_err = bm8563_clock.Init();
        if (rtc_err == ESP_OK) {
            active_clock = &bm8563_clock;
        } else {
            // The time is lost at power-off, but the board still works.
            ESP_LOGE(TAG, "%s: %s, using the %s", bm8563_clock.Name(), esp_err_to_name(rtc_err),
                     timer_clock.Name());
        }
    }
    if (active_clock == &timer_clock) {
        const esp_err_t err = timer_clock.Init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: %s", timer_clock.Name(), esp_err_to_name(err));
            return err;
        }
    }
    rtc::SetSystemTimeFrom(*active_clock);
    return ESP_OK;
}

void BootDone() {
    if (led::Ready()) {
        led::SetIdle(led::colour::kGreen, led::Effect::kBreathe);
    }
}

void LedTyping() {
    if (led::Ready()) {
        led::SetFor(led::colour::kBlue, led::Effect::kSolid, kTypingLedMs);
    }
}

void LedWaitingForUser(UserWait what) {
    if (!led::Ready()) {
        return;
    }
    switch (what) {
        case UserWait::kNone:
            led::EndFor();
            break;
        case UserWait::kTouch:
            led::SetFor(led::colour::kYellow, led::Effect::kNormBlink, kUserWaitLedMs);
            break;
        case UserWait::kLongTouch:
            led::SetFor(led::colour::kYellow, led::Effect::kFastBlink, kUserWaitLedMs);
            break;
        case UserWait::kLongTouchHeld:
            led::SetFor(led::colour::kYellow, led::Effect::kSolid, kUserWaitLedMs);
            break;
    }
}

void SetLedBrightness(uint8_t percent, uint8_t idle_percent) {
    if (led::Ready()) {
        led::SetBrightness(percent, idle_percent);
    }
}

i2cbus::Bus &I2c() { return i2c_bus; }

rtc::Clock &Clock() { return *active_clock; }

buttons::Button Button(ButtonId id) {
    const ButtonPin &pin = kBoard.Button(id);
    return buttons::Button(pin.gpio == kNoPin ? GPIO_NUM_NC : static_cast<gpio_num_t>(pin.gpio),
                           pin.active_low);
}

void PrintClock() {
    printf("[clock]\n");
    printf("hw rtc     %s", Name(kBoard.hw_clock));
    if (kBoard.hw_clock == HwClockType::kBm8563) {
        if (active_clock == &bm8563_clock) {
            printf(" at 0x51");
        } else {
            printf(" (not found)");
        }
    }
    printf("\n");
    printf("source     %s, %s\n", active_clock->Name(),
           active_clock->KeepsTimeWhenOff() ? "keeps time when off" : "lost at power-off");
    const uint64_t timer_us = esp_rtc_get_time_us();
    printf("rtc timer  %" PRIu64 ".%03u s since power-on\n", timer_us / 1000000,
           static_cast<unsigned>(timer_us % 1000000 / 1000));
    int64_t clock_us = 0;
    bool valid = false;
    const esp_err_t err = active_clock->Read(&clock_us, &valid);
    const int64_t system_us = rtc::SystemTimeUs();
    if (err != ESP_OK) {
        printf("clock      read failed: %s\n", esp_err_to_name(err));
    } else if (!valid) {
        printf("clock      no time%s%s%s\n", active_clock == &bm8563_clock ? " (" : "",
               active_clock == &bm8563_clock ? bm8563_clock.State() : "",
               active_clock == &bm8563_clock ? ")" : "");
    } else {
        PrintUtc("clock", clock_us);
    }
    if (rtc::Believable(system_us / 1000000)) {
        PrintUtc("system", system_us);
        if (err == ESP_OK && valid) {
            printf("drift      %+" PRId64 " ms (clock - system)\n", (clock_us - system_us) / 1000);
        }
    } else {
        printf("system     not set\n");
    }
}

void PrintInfo() {
    const Board &b = kBoard;
    printf("board      %s\n", b.name);
    printf("clock      %s (hardware RTC: %s)\n", active_clock->Name(), Name(b.hw_clock));
    printf("display    %s\n", Name(b.display));
    printf("touch      %s\n", Name(b.touch));
    printf("led        %s\n", Name(b.led));
    printf("battery    %s\n", Name(b.battery));
    printf("sd card    %s\n", b.has_sd_card ? "yes" : "no");
    printf("fido2      up to %u credentials, reset ", static_cast<unsigned>(b.fido_max_credentials));
    if (b.fido_reset_window_s != 0) {
        printf("within %u s of start\n", static_cast<unsigned>(b.fido_reset_window_s));
    } else {
        printf("any time\n");
    }
    printf("frontlight %s\n", b.has_frontlight ? "yes" : "no");
    printf("buttons   ");
    for (size_t i = 0; i < kButtonCount; ++i) {
        PrintPin(Name(static_cast<ButtonId>(i)), b.buttons[i].gpio);
    }
    printf("\n");
    printf("pins      ");
    PrintPin("i2c.scl", b.i2c.scl);
    PrintPin("i2c.sda", b.i2c.sda);
    PrintPin("led", b.led_pins.data);
    PrintPin("epd.mosi", b.display_pins.mosi);
    PrintPin("epd.sclk", b.display_pins.sclk);
    PrintPin("epd.cs", b.display_pins.cs);
    PrintPin("epd.dc", b.display_pins.dc);
    PrintPin("epd.rst", b.display_pins.reset);
    PrintPin("epd.busy", b.display_pins.busy);
    PrintPin("touch.rst", b.touch_pins.reset);
    PrintPin("touch.int", b.touch_pins.interrupt);
    PrintPin("touch.pwr", b.touch_pins.power_enable);
    PrintPin("sd.clk", b.sd_pins.clk);
    PrintPin("sd.cmd", b.sd_pins.cmd);
    PrintPin("sd.d0", b.sd_pins.d0);
    PrintPin("sd.pwr", b.sd_pins.power_enable);
    PrintPin("light.cool", b.frontlight_pins.cool);
    PrintPin("light.warm", b.frontlight_pins.warm);
    PrintPin("rail", b.power_pins.peripheral_rail);
    PrintPin("charging", b.power_pins.charger_status);
    printf("\n");
}

}  // namespace boards
