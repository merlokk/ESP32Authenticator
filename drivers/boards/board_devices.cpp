#include "board_devices.h"

#include <cstdio>

#include "esp_log.h"
#include "timer_clock.h"

namespace boards {

namespace {

constexpr const char *TAG = "board";

rtc::TimerClock timer_clock;
rtc::Clock *active_clock = &timer_clock;

void PrintPin(const char *label, int8_t gpio) {
    if (gpio != kNoPin) {
        printf(" %s=%d", label, gpio);
    }
}

}  // namespace

esp_err_t Init() {
    ESP_LOGI(TAG, "%s", kBoard.name);
    if (kBoard.hw_clock != HwClockType::kNone) {
        // TODO: BM8563 driver; until then the time is lost at power-off.
        ESP_LOGW(TAG, "no %s driver yet, using the %s", Name(kBoard.hw_clock), timer_clock.Name());
    }
    const esp_err_t err = active_clock->Init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", active_clock->Name(), esp_err_to_name(err));
        return err;
    }
    rtc::SetSystemTimeFrom(*active_clock);
    return ESP_OK;
}

rtc::Clock &Clock() { return *active_clock; }

buttons::Button Button(ButtonId id) {
    const ButtonPin &pin = kBoard.Button(id);
    return buttons::Button(pin.gpio == kNoPin ? GPIO_NUM_NC : static_cast<gpio_num_t>(pin.gpio),
                           pin.active_low);
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
