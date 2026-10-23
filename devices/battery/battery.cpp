#include "battery.h"

#include <cstdio>
#include <initializer_list>

#include "board.h"
#include "board_devices.h"
#include "cw2017_logic.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace battery {

namespace {

namespace cw = cw2017;

constexpr const char *TAG = "battery";

// VERSION .. SOC_ALERT in one read: a consistent snapshot.
constexpr size_t kStatusSize = cw::kRegSocAlert + 1;

bool gauge_found = false;
bool gauge_ready = false;
cw::Action last_action = cw::Action::kNone;
esp_err_t last_error = ESP_OK;

void Sleep(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

esp_err_t ReadStatus(i2cbus::Lease &lease, uint8_t (&regs)[kStatusSize]) {
    return lease.ReadRegister(cw::kAddress, cw::kRegVersion, regs, sizeof regs);
}

// Byte by byte, as the stock firmware does: a profile is never judged (or
// rewritten) on a block transfer the chip might not auto-increment.
esp_err_t ReadProfile(i2cbus::Lease &lease, uint8_t (&profile)[cw::kBatinfoSize]) {
    for (size_t i = 0; i < cw::kBatinfoSize; ++i) {
        const esp_err_t err = lease.ReadRegister(
            cw::kAddress, static_cast<uint8_t>(cw::kRegBatinfo + i), &profile[i], 1);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t WriteProfile(i2cbus::Lease &lease) {
    for (size_t i = 0; i < cw::kBatinfoSize; ++i) {
        const esp_err_t err = lease.WriteRegister(
            cw::kAddress, static_cast<uint8_t>(cw::kRegBatinfo + i), cw::kBatinfo[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return lease.WriteRegister(cw::kAddress, cw::kRegSocAlert, cw::kUpdateFlag);
}

// One lease for the whole sequence (60 ms): nobody talks to the gauge mid-reset.
esp_err_t SoftReset() {
    auto lease = boards::I2c().Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }
    for (const uint8_t mode : {cw::kModeDefault, cw::kModeRestart, cw::kModeNormal}) {
        const esp_err_t err = lease.WriteRegister(cw::kAddress, cw::kRegMode, mode);
        if (err != ESP_OK) {
            return err;
        }
        Sleep(cw::kResetStepMs);
    }
    return ESP_OK;
}

// One register under a short lease: the bus is free between the polls.
bool Poll(uint8_t reg, uint8_t *value) {
    auto lease = boards::I2c().Acquire();
    return lease && lease.ReadRegister(cw::kAddress, reg, value, 1) == ESP_OK;
}

esp_err_t WaitUntilRunning() {
    uint8_t value = 0;
    int i = 0;
    for (; i < cw::kVersionPolls; ++i) {
        if (Poll(cw::kRegVersion, &value) && cw::VersionRunning(value)) {
            break;
        }
        Sleep(cw::kVersionPollMs);
    }
    if (i == cw::kVersionPolls) {
        return ESP_ERR_TIMEOUT;
    }
    for (i = 0; i < cw::kSocPolls; ++i) {
        if (Poll(cw::kRegSoc, &value) && cw::SocValid(value)) {
            return ESP_OK;
        }
        Sleep(cw::kSocPollMs);
    }
    return ESP_ERR_TIMEOUT;
}

// Read, decide and (when needed) load the profile under one lease, then reset
// and wait with the bus free.
esp_err_t InitGauge() {
    gauge_ready = false;
    {
        auto lease = boards::I2c().Acquire();
        if (!lease) {
            return boards::I2c().Ready() ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
        }
        if (!gauge_found) {
            if (lease.Probe(cw::kAddress) != ESP_OK) {
                return ESP_ERR_NOT_FOUND;
            }
            gauge_found = true;
        }
        uint8_t regs[kStatusSize] = {};
        uint8_t profile[cw::kBatinfoSize] = {};
        esp_err_t err = ReadStatus(lease, regs);
        if (err == ESP_OK) {
            err = ReadProfile(lease, profile);
        }
        if (err != ESP_OK) {
            return err;
        }
        last_action = cw::Decide(regs[cw::kRegMode], regs[cw::kRegVersion], regs[cw::kRegSoc],
                                 cw::ProfileMatches(regs[cw::kRegSocAlert], profile));
        if (last_action == cw::Action::kUpload) {
            ESP_LOGW(TAG, "gauge holds another profile: loading ours");
            err = WriteProfile(lease);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    if (last_action != cw::Action::kNone) {
        Sleep(cw::kResetStepMs);
        esp_err_t err = SoftReset();
        if (err == ESP_OK) {
            err = WaitUntilRunning();
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    gauge_ready = true;
    return ESP_OK;
}

void LogReading() {
    Reading r;
    if (Read(&r) == ESP_OK) {
        ESP_LOGI(TAG, "%s: %u mV, %u %%%s", Name(boards::kBoard.battery),
                 static_cast<unsigned>(r.millivolts), static_cast<unsigned>(r.percent),
                 ChargeState() == Charge::kCharging ? ", charging" : "");
    }
}

}  // namespace

esp_err_t Init() {
    const int8_t stat = boards::kBoard.power_pins.charger_status;
    if (stat != boards::kNoPin) {
        // The STAT output drives the line both ways: no pull.
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << stat;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io);
    }
    if (boards::kBoard.battery == boards::BatteryType::kNone) {
        return ESP_OK;
    }
    return Reinit();
}

esp_err_t Reinit() {
    if (boards::kBoard.battery == boards::BatteryType::kNone) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    last_error = InitGauge();
    if (last_error != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", Name(boards::kBoard.battery), esp_err_to_name(last_error));
        return last_error;
    }
    if (last_action != cw::Action::kNone) {
        ESP_LOGI(TAG, "gauge: %s done", cw::Name(last_action));
    }
    LogReading();
    return ESP_OK;
}

bool GaugeReady() { return gauge_ready; }

esp_err_t Read(Reading *out) {
    if (!gauge_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t regs[kStatusSize] = {};
    {
        auto lease = boards::I2c().Acquire();
        if (!lease) {
            return ESP_ERR_TIMEOUT;
        }
        const esp_err_t err = ReadStatus(lease, regs);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (regs[cw::kRegMode] != cw::kModeNormal || !cw::VersionRunning(regs[cw::kRegVersion]) ||
        !cw::SocValid(regs[cw::kRegSoc])) {
        return ESP_ERR_INVALID_STATE;
    }
    out->millivolts = cw::VcellMillivolts(regs[cw::kRegVcell], regs[cw::kRegVcell + 1]);
    out->percent = regs[cw::kRegSoc];
    out->fraction = regs[cw::kRegSocFrac];
    return ESP_OK;
}

Charge ChargeState() {
    const int8_t stat = boards::kBoard.power_pins.charger_status;
    if (stat == boards::kNoPin) {
        return Charge::kUnknown;
    }
    return gpio_get_level(static_cast<gpio_num_t>(stat)) != 0 ? Charge::kCharging
                                                              : Charge::kNotCharging;
}

int Level() {
    Reading r;
    return Read(&r) == ESP_OK ? r.percent : -1;
}

void PrintInfo() {
    const boards::Board &b = boards::kBoard;
    if (b.battery == boards::BatteryType::kNone) {
        printf("gauge      none on this board\n");
    } else if (!gauge_ready) {
        printf("gauge      %s at 0x%02x: %s\n", Name(b.battery), cw::kAddress,
               esp_err_to_name(last_error != ESP_OK ? last_error : ESP_ERR_INVALID_STATE));
    } else {
        printf("gauge      %s at 0x%02x, profile ok (init: %s)\n", Name(b.battery), cw::kAddress,
               cw::Name(last_action));
        Reading r;
        const esp_err_t err = Read(&r);
        if (err != ESP_OK) {
            printf("reading    %s (try `battery init`)\n", esp_err_to_name(err));
        } else {
            printf("voltage    %u mV\n", static_cast<unsigned>(r.millivolts));
            printf("level      %u.%u %%\n", static_cast<unsigned>(r.percent),
                   static_cast<unsigned>(r.fraction * 10 / 256));
        }
    }
    switch (ChargeState()) {
        case Charge::kUnknown:
            printf("charging   unknown (no STAT line)\n");
            break;
        case Charge::kCharging:
            printf("charging   yes (GPIO%d high)\n", b.power_pins.charger_status);
            break;
        case Charge::kNotCharging:
            printf("charging   no (GPIO%d low: no charger, or full)\n", b.power_pins.charger_status);
            break;
    }
}

void PrintRegs() {
    if (!gauge_found) {
        printf("no gauge\n");
        return;
    }
    uint8_t regs[16] = {};
    uint8_t profile[cw::kBatinfoSize] = {};
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (auto lease = boards::I2c().Acquire()) {
        err = lease.ReadRegister(cw::kAddress, 0x00, regs, sizeof regs);
        if (err == ESP_OK) {
            err = ReadProfile(lease, profile);
        }
    }
    if (err != ESP_OK) {
        printf("read: %s\n", esp_err_to_name(err));
        return;
    }
    printf("0x00:");
    for (const uint8_t v : regs) {
        printf(" %02x", v);
    }
    printf("\n");
    for (size_t i = 0; i < cw::kBatinfoSize; i += 16) {
        printf("0x%02x:", static_cast<unsigned>(cw::kRegBatinfo + i));
        for (size_t j = i; j < i + 16; ++j) {
            printf(" %02x", profile[j]);
        }
        printf("\n");
    }
    printf("profile    %s\n", cw::ProfileMatches(regs[cw::kRegSocAlert], profile)
                                  ? "ours (update flag set)"
                                  : "not ours");
}

}  // namespace battery
