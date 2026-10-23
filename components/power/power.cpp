#include "power.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "epd.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "frontlight.h"
#include "keypad.h"
#include "power_logic.h"
#include "ui.h"

namespace power {

namespace {

constexpr const char *TAG = "power";

// lodepng and the dither run here: heap stack, the task never ends.
constexpr uint32_t kTaskStackBytes = 6144;
constexpr int kTaskPriority = 4;

bool requested = false;

gpio_num_t PowerPin() {
    return static_cast<gpio_num_t>(boards::kBoard.Button(boards::ButtonId::kPower).gpio);
}

void Task(void *) { Off(); }

}  // namespace

void Boot() {
    const Holds holds = SleepHolds(boards::kBoard);
    for (size_t i = 0; i < holds.size; ++i) {
        gpio_hold_dis(static_cast<gpio_num_t>(holds.items[i].pin));
    }
    gpio_deep_sleep_hold_dis();
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_EXT1)) {
        ESP_LOGI(TAG, "woken by the Power button");
    }
}

esp_err_t RequestOff() {
    if (boards::kBoard.Button(boards::ButtonId::kPower).gpio == boards::kNoPin) {
        return ESP_ERR_NOT_SUPPORTED;  // nothing would wake it
    }
    if (requested) {
        return ESP_ERR_INVALID_STATE;
    }
    requested = true;
    return xTaskCreate(&Task, "power_off", kTaskStackBytes, nullptr, kTaskPriority, nullptr) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void Off() {
    ESP_LOGI(TAG, "powering off");
    ui::Lock();  // never given back: the ui task draws nothing more
    frontlight::Off();
    const esp_err_t err = ui::ShowWallpaper();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wallpaper: %s", esp_err_to_name(err));
    }
    epd::Sleep();
    // Power held is a low level: ext1 would wake at once.
    while (keypad::Pressed(boards::ButtonId::kPower)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const gpio_num_t power = PowerPin();
    const Holds holds = SleepHolds(boards::kBoard);
    for (size_t i = 0; i < holds.size; ++i) {
        const auto pin = static_cast<gpio_num_t>(holds.items[i].pin);
        gpio_hold_dis(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, holds.items[i].level);
        gpio_hold_en(pin);
    }
    esp_sleep_config_gpio_isolate();
    // The button's pull-up through sleep: an RTC pull, RTC peripherals on.
    rtc_gpio_pullup_en(power);
    rtc_gpio_pulldown_dis(power);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_ext1_wakeup_io(1ULL << power, ESP_EXT1_WAKEUP_ANY_LOW);
    gpio_deep_sleep_hold_en();
    ESP_LOGI(TAG, "deep sleep");
    esp_deep_sleep_start();
}

}  // namespace power
