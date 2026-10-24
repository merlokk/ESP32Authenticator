#include "touch.h"

#include <cstdio>
#include <cstring>

#include "board.h"
#include "board_devices.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace touch {

namespace {

namespace gt = gt911;

constexpr const char *TAG = "touch";

uint8_t address = 0;
bool home = false;
gt::Info info = {};
gt::ConfigHead config = {};
bool have_config = false;
Snapshot snapshot = {};
gt::Edges edges;  // under `lock`
// The keypad task writes the snapshot, the console reads it.
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

void Sleep(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1); }

void Output(int8_t pin, int level) {
    const auto gpio = static_cast<gpio_num_t>(pin);
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, level);
}

// RST low with INT at the address level, RST high, INT held, then INT
// released (input) and the config load.
void Reset(int int_level) {
    const boards::TouchPins &p = boards::kBoard.touch_pins;
    Output(p.interrupt, int_level);
    Output(p.reset, 0);
    Sleep(gt::kResetLowMs);
    gpio_set_level(static_cast<gpio_num_t>(p.reset), 1);
    Sleep(gt::kResetHighMs);
    Sleep(gt::kIntHoldMs);
    gpio_set_direction(static_cast<gpio_num_t>(p.interrupt), GPIO_MODE_INPUT);
    Sleep(gt::kConfigLoadMs);
}

esp_err_t Read(i2cbus::Lease &lease, uint16_t reg, uint8_t *out, size_t size) {
    const uint8_t r[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)};
    return lease.WriteRead(address, r, sizeof r, out, size);
}

esp_err_t Write8(i2cbus::Lease &lease, uint16_t reg, uint8_t value) {
    const uint8_t w[3] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF), value};
    return lease.Write(address, w, sizeof w);
}

bool Probe(uint8_t a) {
    auto lease = boards::I2c().Acquire();
    return lease && lease.Probe(a) == ESP_OK;
}

}  // namespace

esp_err_t Init() {
    const boards::Board &b = boards::kBoard;
    if (b.touch != boards::TouchType::kGt911 || address != 0) {
        return address != 0 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    if (!boards::I2c().Ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (b.touch_pins.power_enable != boards::kNoPin) {
        Output(b.touch_pins.power_enable, 0);  // active low: on
        Sleep(gt::kPowerSettleMs);
    }
    // INT low selects 0x5D; a module strapped the other way answers at 0x14.
    Reset(0);
    if (Probe(gt::kAddress)) {
        address = gt::kAddress;
    } else {
        Reset(1);
        address = Probe(gt::kAddressAlt) ? gt::kAddressAlt : Probe(gt::kAddress) ? gt::kAddress : 0;
    }
    if (address == 0) {
        ESP_LOGE(TAG, "GT911: no answer at 0x%02x or 0x%02x", gt::kAddress, gt::kAddressAlt);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t raw_info[gt::kInfoSize] = {};
    uint8_t raw_config[gt::kConfigHeadSize] = {};
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (auto lease = boards::I2c().Acquire()) {
        err = Read(lease, gt::kRegProductId, raw_info, sizeof raw_info);
        if (err == ESP_OK && Read(lease, gt::kRegConfig, raw_config, sizeof raw_config) == ESP_OK) {
            config = gt::DecodeConfigHead(raw_config);
            have_config = true;
        }
        Write8(lease, gt::kRegStatus, 0);  // drop a frame from before
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 at 0x%02x: info: %s", address, esp_err_to_name(err));
        address = 0;
        return err;
    }
    info = gt::DecodeInfo(raw_info);
    ESP_LOGI(TAG, "GT%s at 0x%02x, firmware 0x%04x, %ux%u", info.product, address, info.firmware,
             info.x_max, info.y_max);
    return ESP_OK;
}

bool Present() { return address != 0; }

esp_err_t Poll() {
    if (address == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t status = 0;
    uint8_t points[gt::kMaxPoints * gt::kPointSize];
    uint8_t count = 0;
    esp_err_t err = ESP_ERR_TIMEOUT;
    {
        // One lease: status, contacts and the clear belong to one frame.
        auto lease = boards::I2c().Acquire();
        if (lease) {
            err = Read(lease, gt::kRegStatus, &status, 1);
            const gt::Frame f = gt::Decode(status, home);
            if (err == ESP_OK && f.fresh) {
                count = f.touches > gt::kMaxPoints ? gt::kMaxPoints : f.touches;
                if (count > 0) {
                    err = Read(lease, gt::kRegPoints, points, count * gt::kPointSize);
                }
                const esp_err_t clear = Write8(lease, gt::kRegStatus, 0);
                if (err == ESP_OK) {
                    err = clear;
                }
            }
        }
    }
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&lock);
    if (err != ESP_OK) {
        ++snapshot.errors;
    } else {
        const gt::Frame f = gt::Decode(status, home);
        home = f.home;
        if (f.fresh) {
            ++snapshot.frames;
            snapshot.at_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            snapshot.status = status;
            snapshot.count = count;
            memcpy(snapshot.raw, points, count * gt::kPointSize);
        }
    }
    // Every poll, a failed read too: a lost lift frame still ends the touch.
    const bool down = gt::Touching(snapshot.count, now - snapshot.at_ms);
    const gt::Point p = down ? gt::DecodePoint(snapshot.raw) : gt::Point{};
    edges.Update(down, p.x, p.y);
    portEXIT_CRITICAL(&lock);
    return err;
}

bool HomePressed() { return home; }

bool Contact(gt::Point *out) {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&lock);
    const bool down = gt::Touching(snapshot.count, now - snapshot.at_ms);
    if (down) {
        *out = gt::DecodePoint(snapshot.raw);
    }
    portEXIT_CRITICAL(&lock);
    return down;
}

bool NextEdge(gt::Edge *out, uint8_t *more) {
    portENTER_CRITICAL(&lock);
    const bool got = edges.Pop(out);
    if (more != nullptr) {
        *more = edges.Pending();
    }
    portEXIT_CRITICAL(&lock);
    return got;
}

Snapshot GetSnapshot() {
    portENTER_CRITICAL(&lock);
    const Snapshot copy = snapshot;
    portEXIT_CRITICAL(&lock);
    return copy;
}

void PrintInfo() {
    const boards::Board &b = boards::kBoard;
    if (b.touch == boards::TouchType::kNone) {
        printf("touch      none on this board\n");
        return;
    }
    if (address == 0) {
        printf("touch      GT911 not found (power GPIO%d, rst GPIO%d, int GPIO%d)\n",
               b.touch_pins.power_enable, b.touch_pins.reset, b.touch_pins.interrupt);
        return;
    }
    printf("touch      GT%s at 0x%02x, firmware 0x%04x, vendor (module) 0x%02x\n", info.product,
           address, info.firmware, info.vendor);
    printf("panel      %u x %u (info block)\n", info.x_max, info.y_max);
    if (have_config) {
        printf("config     version 0x%02x, output %u x %u, up to %u contacts, switch1 0x%02x"
               " (INT %s%s)\n",
               config.version, config.x_max, config.y_max, config.touches, config.switch1,
               (config.switch1 & 0x03) == 0   ? "rising"
               : (config.switch1 & 0x03) == 1 ? "falling"
               : (config.switch1 & 0x03) == 2 ? "low"
                                              : "high",
               (config.switch1 & 0x08) != 0 ? ", X/Y swapped" : "");
    }
    const Snapshot s = GetSnapshot();
    printf("frames     %lu new, %lu read errors; home %s\n", static_cast<unsigned long>(s.frames),
           static_cast<unsigned long>(s.errors), home ? "touched" : "not touched");
}

}  // namespace touch
