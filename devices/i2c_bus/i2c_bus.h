#pragma once

// The board's one I2C bus, leased rather than shared. Ported from
// approver-esp32 (components/i2cbus).
//
// The X4 Pro has the BM8563 RTC, the GT911 touch and the CW2017 fuel gauge on
// two wires, and several tasks want them (console, BLE, UI). So there is
// exactly one owner (boards::I2c()), and everybody else borrows:
//
//     if (auto lease = bus.Acquire()) {
//         lease.WriteRegister(kAddr, 0x30, value);   // a sequence, uninterrupted
//         lease.ReadRegister(kAddr, 0x34, buf, 2);
//     }                                              // released here, on every path
//
// **The lease exists to make a sequence atomic**, not a single transfer. One
// transaction needs no help; a read-modify-write or a reset sequence does.
//
// A failed acquire is a logged skip, never a block: `Held()` is false and every
// transfer returns ESP_ERR_INVALID_STATE, and the caller decides what a miss
// means. A task waiting forever on a wedged bus is a watchdog panic with a
// confusing name.
//
// It knows about wires, not about boards or chips.

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace i2cbus {

// The default clock. **Speed is per device**, not one number for the wire:
// `driver/i2c_master.h` puts `scl_speed_hz` in the *device* config, so a slow
// chip does not slow the others down. A device that says nothing gets this.
inline constexpr uint32_t kClockHz = 400000;

// Long enough that a slow chip finishes, short enough that a wedged one does
// not take the frame with it.
inline constexpr uint32_t kDefaultAcquireMs = 50;
inline constexpr int kTransferTimeoutMs = 100;

// What `Recover` waits for the bus. Longer than an ordinary acquire because a
// recovery is rare and the holder it is waiting on is, by definition, the task
// that is stuck — and still bounded, because a recovery that blocks forever is
// the watchdog panic this whole design exists to avoid.
inline constexpr uint32_t kRecoverAcquireMs = 250;

// Device handles are cached per address in a fixed table: no heap of ours.
inline constexpr size_t kMaxDevices = 8;

class Bus;

// The scope guard. Non-copyable, movable, and releasing is the destructor —
// "release" is not a line anyone can forget to write.
class Lease {
   public:
    explicit Lease(Bus *bus) : bus_(bus) {}
    ~Lease();

    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept : bus_(other.bus_) { other.bus_ = nullptr; }
    Lease &operator=(Lease &&) = delete;

    bool Held() const { return bus_ != nullptr; }
    explicit operator bool() const { return Held(); }

    esp_err_t Probe(uint8_t address);
    esp_err_t Write(uint8_t address, const uint8_t *data, size_t length);
    esp_err_t Read(uint8_t address, uint8_t *out, size_t length);
    esp_err_t WriteRead(uint8_t address, const uint8_t *write, size_t write_length,
                        uint8_t *read, size_t read_length);

    // The two shapes every chip on this board actually uses.
    esp_err_t ReadRegister(uint8_t address, uint8_t reg, uint8_t *out, size_t length);
    esp_err_t WriteRegister(uint8_t address, uint8_t reg, uint8_t value);

   private:
    Bus *bus_;
};

class Bus {
   public:
    Bus() = default;
    Bus(const Bus &) = delete;
    Bus &operator=(const Bus &) = delete;

    // Trivial constructor, separate Init: a static object whose constructor
    // touched I2C would be a boot crash naming the wrong thing.
    //
    // The pins are arguments, not an include of `board.h`: this layer knows
    // about wires, not about which board they are on.
    esp_err_t Init(gpio_num_t scl, gpio_num_t sda, uint32_t clock_hz = kClockHz);
    bool Ready() const { return handle_ != nullptr; }

    // Declares the clock a particular chip is talked to at. Call it before the
    // first transfer to that address — and **not** while holding a lease, since
    // it takes the bus itself. A chip that never calls this runs at kClockHz.
    esp_err_t AddDevice(uint8_t address, uint32_t clock_hz);

    Lease Acquire(uint32_t timeout_ms = kDefaultAcquireMs);

    // The raw bus handle, for a third-party driver that has to open its own
    // device on this bus (e.g. `esp_lcd_touch`).
    //
    // **It is not permission to skip the lease.** Whoever calls such a driver
    // takes the lease around the call, so the driver's transfers land inside a
    // critical section it has never heard of.
    i2c_master_bus_handle_t Handle() const { return handle_; }

    // A slave holding SDA low is a known failure with a known fix: clock it out
    // until it lets go, then re-init. Handled once, here, rather than in
    // every driver. Bounded, and one log line.
    //
    // **It takes the lease itself, so do not call it holding one** — the same
    // rule as `AddDevice` above, and for a sharper reason: this one removes
    // every device handle and deletes the driver, so running it beside another
    // task's transfer would hand that task a handle that has been freed. A bus
    // it cannot get is `ESP_ERR_TIMEOUT` and nothing torn down.
    esp_err_t Recover();

   private:
    friend class Lease;

    void Release();
    esp_err_t DeviceFor(uint8_t address, i2c_master_dev_handle_t *out);
    esp_err_t OpenDevice(uint8_t address, uint32_t clock_hz, i2c_master_dev_handle_t *out);
    void ForgetDevices();

    struct DeviceSlot {
        uint8_t address;
        bool used;
        uint32_t clock_hz;
        i2c_master_dev_handle_t handle;
    };

    i2c_master_bus_handle_t handle_ = nullptr;
    gpio_num_t scl_ = GPIO_NUM_NC;
    gpio_num_t sda_ = GPIO_NUM_NC;
    uint32_t clock_hz_ = kClockHz;
    SemaphoreHandle_t mutex_ = nullptr;
    StaticSemaphore_t mutex_storage_ = {};
    DeviceSlot devices_[kMaxDevices] = {};
};

}  // namespace i2cbus
