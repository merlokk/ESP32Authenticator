#include "frontlight.h"

#include <cstdio>

#include "board.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "frontlight_logic.h"

namespace frontlight {

namespace {

constexpr const char *TAG = "light";
constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kCool = LEDC_CHANNEL_0;
constexpr ledc_channel_t kWarm = LEDC_CHANNEL_1;

bool ready = false;
uint8_t brightness = 0;
uint8_t warmth = 50;
Duties duties = {0, 0};

esp_err_t Channel(ledc_channel_t channel, int8_t gpio) {
    ledc_channel_config_t c = {};
    c.gpio_num = gpio;
    c.speed_mode = kMode;
    c.channel = channel;
    c.timer_sel = kTimer;
    c.duty = 0;  // dark until told otherwise
    c.hpoint = 0;
    return ledc_channel_config(&c);
}

// Fades one channel to `duty`; a fade still running is cut first.
esp_err_t FadeTo(ledc_channel_t channel, uint32_t duty) {
    ledc_fade_stop(kMode, channel);
    esp_err_t err = ledc_set_fade_with_time(kMode, channel, duty, static_cast<int>(kFadeMs));
    if (err == ESP_OK) {
        err = ledc_fade_start(kMode, channel, LEDC_FADE_NO_WAIT);
    }
    return err;
}

}  // namespace

esp_err_t Init() {
    const boards::Board &b = boards::kBoard;
    if (!b.has_frontlight || ready) {
        return ready ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    ledc_timer_config_t timer = {};
    timer.speed_mode = kMode;
    timer.duty_resolution = static_cast<ledc_timer_bit_t>(kResolutionBits);
    timer.timer_num = kTimer;
    timer.freq_hz = kPwmHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer);
    if (err == ESP_OK) {
        err = Channel(kCool, b.frontlight_pins.cool);
    }
    if (err == ESP_OK) {
        err = Channel(kWarm, b.frontlight_pins.warm);
    }
    if (err == ESP_OK) {
        err = ledc_fade_func_install(0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc: %s", esp_err_to_name(err));
        return err;
    }
    ready = true;
    return ESP_OK;
}

bool Present() { return ready; }

esp_err_t Set(uint8_t new_brightness, uint8_t new_warmth) {
    if (!ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    brightness = new_brightness > 100 ? 100 : new_brightness;
    warmth = new_warmth > 100 ? 100 : new_warmth;
    duties = Mix(brightness, warmth);
    esp_err_t err = FadeTo(kCool, duties.cool);
    const esp_err_t warm_err = FadeTo(kWarm, duties.warm);
    if (err == ESP_OK) {
        err = warm_err;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fade: %s", esp_err_to_name(err));
    }
    return err;
}

uint8_t Brightness() { return brightness; }

uint8_t Warmth() { return warmth; }

void PrintInfo() {
    const boards::Board &b = boards::kBoard;
    if (!b.has_frontlight) {
        printf("light      none on this board\n");
        return;
    }
    if (!ready) {
        printf("light      not running (LEDC did not come up)\n");
        return;
    }
    printf("light      %s, brightness %u%%, warmth %u%%\n", brightness == 0 ? "off" : "on",
           static_cast<unsigned>(brightness), static_cast<unsigned>(warmth));
    printf("pwm        %u kHz, %u bits; cool GPIO%d duty %u, warm GPIO%d duty %u (of %u)\n",
           static_cast<unsigned>(kPwmHz / 1000), static_cast<unsigned>(kResolutionBits),
           b.frontlight_pins.cool, static_cast<unsigned>(duties.cool), b.frontlight_pins.warm,
           static_cast<unsigned>(duties.warm), static_cast<unsigned>(kFullDuty));
}

}  // namespace frontlight
