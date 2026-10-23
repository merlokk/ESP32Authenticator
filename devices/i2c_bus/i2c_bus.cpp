#include "i2c_bus.h"

#include <cinttypes>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

namespace i2cbus {

namespace {

constexpr const char *TAG = "i2c";

// Nine pulses is the standard unwedge: the longest a slave can be mid-byte.
constexpr int kRecoveryPulses = 9;
constexpr int kRecoveryHalfPeriodUs = 5;

}  // namespace

Lease::~Lease() {
    if (bus_ != nullptr) {
        bus_->Release();
        bus_ = nullptr;
    }
}

esp_err_t Bus::Init(gpio_num_t scl, gpio_num_t sda, uint32_t clock_hz) {
    if (handle_ != nullptr) {
        return ESP_OK;
    }

    scl_ = scl;
    sda_ = sda;
    clock_hz_ = clock_hz;

    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    }
    if (mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // The new driver (`driver/i2c_master.h`), not the legacy `driver/i2c.h`:
    // deprecated on 5.x, and the handle-per-device model is what a lease wants
    // anyway.
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_,
        .scl_io_num = scl_,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags =
            {
                .enable_internal_pullup = true,
                .allow_pd = false,
            },
    };

    const esp_err_t err = i2c_new_master_bus(&config, &handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
        handle_ = nullptr;
        return err;
    }

    ESP_LOGI(TAG, "bus up on scl=%d sda=%d at %" PRIu32 " Hz", scl_, sda_, clock_hz_);
    return ESP_OK;
}

Lease Bus::Acquire(uint32_t timeout_ms) {
    if (handle_ == nullptr || mutex_ == nullptr) {
        return Lease(nullptr);
    }
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        // A miss is a logged skip. The caller decides what it means: the clock
        // keeps its last value, a touch read drops the frame.
        ESP_LOGW(TAG, "bus busy, skipped after %" PRIu32 " ms", timeout_ms);
        return Lease(nullptr);
    }
    return Lease(this);
}

void Bus::Release() { xSemaphoreGive(mutex_); }

esp_err_t Bus::OpenDevice(uint8_t address, uint32_t clock_hz,
                          i2c_master_dev_handle_t *out) {
    for (DeviceSlot &slot : devices_) {
        if (slot.used) {
            continue;
        }
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = clock_hz,
            .scl_wait_us = 0,
            .flags = {.disable_ack_check = false},
        };
        const esp_err_t err = i2c_master_bus_add_device(handle_, &config, &slot.handle);
        if (err != ESP_OK) {
            return err;
        }
        slot.address = address;
        slot.clock_hz = clock_hz;
        slot.used = true;
        if (out != nullptr) {
            *out = slot.handle;
        }
        return ESP_OK;
    }

    // Fixed capacity makes "full" a state that is designed, not reported later.
    // Three chips, eight slots: this is a wiring mistake, not load.
    ESP_LOGE(TAG, "no slot left for 0x%02x", address);
    return ESP_ERR_NO_MEM;
}

esp_err_t Bus::AddDevice(uint8_t address, uint32_t clock_hz) {
    if (handle_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto lease = Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    for (DeviceSlot &slot : devices_) {
        if (!slot.used || slot.address != address) {
            continue;
        }
        if (slot.clock_hz == clock_hz) {
            return ESP_OK;
        }
        // Already open at another speed: the handle carries the clock, so it
        // has to be reopened rather than adjusted.
        i2c_master_bus_rm_device(slot.handle);
        slot.used = false;
        slot.handle = nullptr;
        break;
    }

    const esp_err_t err = OpenDevice(address, clock_hz, nullptr);
    if (err == ESP_OK && clock_hz != kClockHz) {
        ESP_LOGI(TAG, "0x%02x runs at %" PRIu32 " Hz, not the bus default", address,
                 clock_hz);
    }
    return err;
}

esp_err_t Bus::DeviceFor(uint8_t address, i2c_master_dev_handle_t *out) {
    for (DeviceSlot &slot : devices_) {
        if (slot.used && slot.address == address) {
            *out = slot.handle;
            return ESP_OK;
        }
    }
    return OpenDevice(address, clock_hz_, out);
}

void Bus::ForgetDevices() {
    for (DeviceSlot &slot : devices_) {
        if (slot.used) {
            i2c_master_bus_rm_device(slot.handle);
            slot.used = false;
            slot.handle = nullptr;
        }
    }
}

esp_err_t Bus::Recover() {
    if (handle_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // **Under the lease, like everything else that touches the wire.** This is
    // the most destructive thing in the class — it removes every device handle
    // and deletes the driver — so doing it while another task is mid-transfer
    // hands that task a handle that no longer exists. On one core with
    // preemption that is a use-after-free, not a race that "usually works".
    //
    // Longer than the default acquire: a recovery is rare and worth waiting
    // for, and the task it is waiting on is by definition the one that is
    // stuck. It is still a timeout rather than a block — and the
    // consequence is that `Recover` may **not** be called from inside a lease,
    // because the mutex is not recursive. The header says so.
    auto lease = Acquire(kRecoverAcquireMs);
    if (!lease) {
        ESP_LOGE(TAG, "recovery skipped: the bus is held by somebody else");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGW(TAG, "recovering the bus: clocking SDA free");

    ForgetDevices();
    i2c_del_master_bus(handle_);
    handle_ = nullptr;

    const gpio_config_t scl = {
        .pin_bit_mask = 1ULL << scl_,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&scl);

    for (int i = 0; i < kRecoveryPulses; ++i) {
        gpio_set_level(scl_, 0);
        esp_rom_delay_us(kRecoveryHalfPeriodUs);
        gpio_set_level(scl_, 1);
        esp_rom_delay_us(kRecoveryHalfPeriodUs);
    }

    return Init(scl_, sda_, clock_hz_);
}

// --- The lease's transfers -----------------------------------------------

esp_err_t Lease::Probe(uint8_t address) {
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_probe(bus_->handle_, address, kTransferTimeoutMs);
}

esp_err_t Lease::Write(uint8_t address, const uint8_t *data, size_t length) {
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_dev_handle_t device = nullptr;
    const esp_err_t err = bus_->DeviceFor(address, &device);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_transmit(device, data, length, kTransferTimeoutMs);
}

esp_err_t Lease::Read(uint8_t address, uint8_t *out, size_t length) {
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_dev_handle_t device = nullptr;
    const esp_err_t err = bus_->DeviceFor(address, &device);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_receive(device, out, length, kTransferTimeoutMs);
}

esp_err_t Lease::WriteRead(uint8_t address, const uint8_t *write, size_t write_length,
                           uint8_t *read, size_t read_length) {
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_dev_handle_t device = nullptr;
    const esp_err_t err = bus_->DeviceFor(address, &device);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_transmit_receive(device, write, write_length, read, read_length,
                                       kTransferTimeoutMs);
}

esp_err_t Lease::ReadRegister(uint8_t address, uint8_t reg, uint8_t *out, size_t length) {
    return WriteRead(address, &reg, 1, out, length);
}

esp_err_t Lease::WriteRegister(uint8_t address, uint8_t reg, uint8_t value) {
    const uint8_t payload[2] = {reg, value};
    return Write(address, payload, sizeof(payload));
}

}  // namespace i2cbus
