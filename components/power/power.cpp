#include "power.h"

#include <atomic>
#include <cstring>
#include <ctime>

#include "ble.h"
#include "board.h"
#include "clock_math.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "epd.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
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
constexpr uint32_t kIdleStackBytes = 2560;
constexpr int kIdlePriority = 2;
constexpr uint32_t kIdlePollMs = 5000;

bool requested = false;
bool request_lock = false;
uint32_t request_wake_s = 0;
void (*sleep_hook)(bool lock) = nullptr;
std::atomic<uint64_t> last_activity_ms{0};

// Written right before a deep sleep; Boot moves it to `taken` and wipes it.
RTC_NOINIT_ATTR Kept rtc_kept;
Kept taken;  // until TakeKept (or Start: nobody asked)
bool have_taken = false;

uint64_t NowMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

void Zero(void *p, size_t size) {
    volatile uint8_t *b = static_cast<volatile uint8_t *>(p);
    while (size-- > 0) {
        *b++ = 0;
    }
}

int64_t BelievableNow() {
    const time_t now = time(nullptr);
    return rtc::Believable(now) ? static_cast<int64_t>(now) : 0;
}

gpio_num_t PowerPin() {
    return static_cast<gpio_num_t>(boards::kBoard.Button(boards::ButtonId::kPower).gpio);
}

void Task(void *) { Off(request_lock, request_wake_s); }

// A USB host (the console, a PC) or the pairing window keeps it on, and
// counts as activity: the idle time starts when it ends.
bool Held() {
    return usb_serial_jtag_is_connected() || ble::PairingSecondsLeft() > 0;
}

void IdleTask(void *) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kIdlePollMs));
        const bool held = Held();
        if (held) {
            NoteActivity();
        }
        const uint16_t minutes = config::Get().power.idle_minutes;
        if (IdleOff(NowMs(), last_activity_ms.load(), minutes, held)) {
            ESP_LOGI(TAG, "idle for %u min", static_cast<unsigned>(minutes));
            RequestOff();
        }
    }
}

}  // namespace

void Boot() {
    const Holds holds = SleepHolds(boards::kBoard);
    // Driven to the held level first: a released pad would float, and the
    // peripheral rail with it would cut the battery power.
    for (size_t i = 0; i < holds.size; ++i) {
        const auto pin = static_cast<gpio_num_t>(holds.items[i].pin);
        gpio_set_level(pin, holds.items[i].level);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_hold_dis(pin);
    }
    gpio_deep_sleep_hold_dis();
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_EXT1)) {
        ESP_LOGI(TAG, "woken by the Power button");
    }
    // Only a deep-sleep wake may use it; any other reset finds noise or a
    // copy nobody asked for.
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP && rtc_kept.magic == kKeptMagic) {
        taken = rtc_kept;
        have_taken = true;
    }
    Zero(&rtc_kept, sizeof rtc_kept);
}

esp_err_t Start() {
    NoteActivity();
    if (have_taken) {  // nobody took it (no store): gone
        Zero(&taken, sizeof taken);
        have_taken = false;
    }
    return xTaskCreate(&IdleTask, "power_idle", kIdleStackBytes, nullptr, kIdlePriority,
                       nullptr) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void NoteActivity() { last_activity_ms.store(NowMs()); }

esp_err_t RequestOff(bool lock, uint32_t wake_seconds) {
    if (boards::kBoard.Button(boards::ButtonId::kPower).gpio == boards::kNoPin) {
        return ESP_ERR_NOT_SUPPORTED;  // nothing would wake it
    }
    if (requested) {
        return ESP_ERR_INVALID_STATE;
    }
    requested = true;
    request_lock = lock;
    request_wake_s = wake_seconds;
    return xTaskCreate(&Task, "power_off", kTaskStackBytes, nullptr, kTaskPriority, nullptr) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void SetSleepHook(void (*hook)(bool lock)) { sleep_hook = hook; }

bool Keep(const void *data, size_t size, uint32_t hours) {
    Kept k;
    if (!FillKept(&k, static_cast<const uint8_t *>(data), size, BelievableNow(), hours)) {
        Zero(&k, sizeof k);
        return false;
    }
    rtc_kept = k;
    Zero(&k, sizeof k);
    ESP_LOGI(TAG, "the PIN is kept for %u h of sleep", static_cast<unsigned>(hours));
    return true;
}

bool TakeKept(void *data, size_t size) {
    const bool ok = have_taken && taken.size == size && KeptUsable(taken, BelievableNow());
    if (ok) {
        memcpy(data, taken.data, size);
    } else if (have_taken) {
        ESP_LOGI(TAG, "the kept PIN is out of time (or the clock is lost)");
    }
    Zero(&taken, sizeof taken);
    have_taken = false;
    return ok;
}

void Off(bool lock, uint32_t wake_seconds) {
    ESP_LOGI(TAG, "powering off%s", lock ? ", locked" : "");
    ui::Lock();  // never given back: the ui task draws nothing more
    ui::HoldPanel(true);  // nor its last frame, wallpaper or not
    frontlight::Off();
    if (ble::IsOn()) {
        ble::Off();  // the host sees the link closed, not lost
    }
    const esp_err_t err = ui::ShowWallpaper(config::Get().display.wallpaper ==
                                            config::WallpaperMode::kGrey);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wallpaper: %s", esp_err_to_name(err));
    }
    epd::Sleep();
    // Power held is a low level: ext1 would wake at once.
    while (keypad::Pressed(boards::ButtonId::kPower)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    Zero(&rtc_kept, sizeof rtc_kept);
    if (sleep_hook != nullptr) {
        sleep_hook(lock);
    }
    const gpio_num_t power = PowerPin();
    const Holds holds = SleepHolds(boards::kBoard, config::Get().power.hold_rail);
    for (size_t i = 0; i < holds.size; ++i) {
        const auto pin = static_cast<gpio_num_t>(holds.items[i].pin);
        gpio_hold_dis(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);  // off the PWM: a plain output
        gpio_set_level(pin, holds.items[i].level);
        gpio_hold_en(pin);
    }
    esp_sleep_config_gpio_isolate();
    // The button's pull-up through sleep: an RTC pull, RTC peripherals on.
    rtc_gpio_pullup_en(power);
    rtc_gpio_pulldown_dis(power);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_ext1_wakeup_io(1ULL << power, ESP_EXT1_WAKEUP_ANY_LOW);
    if (wake_seconds != 0) {
        esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(wake_seconds) * 1000000);
    }
    gpio_deep_sleep_hold_en();
    ESP_LOGI(TAG, "deep sleep");
    const esp_err_t refused = esp_deep_sleep_try_to_start();
    // Refused (a wake source already active): awake at full current with the
    // ui locked; start again rather than drain the battery.
    Zero(&rtc_kept, sizeof rtc_kept);
    ESP_LOGE(TAG, "deep sleep refused (%s): restarting", esp_err_to_name(refused));
    esp_restart();
}

}  // namespace power
