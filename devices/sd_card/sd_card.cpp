#include "sd_card.h"

#include <cstdio>
#include <cstring>

#include "board.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"
#include "sdmmc_cmd.h"

namespace sd_card {

namespace {

constexpr const char *TAG = "sd";
constexpr size_t kSectorSize = 512;

sdmmc_card_t *card = nullptr;
Outcome status = Outcome::kNoAnswer;

void Power(bool on) {
    const int8_t pin = boards::kBoard.sd_pins.power_enable;
    if (pin != boards::kNoPin) {
        gpio_set_level(static_cast<gpio_num_t>(pin), on ? 0 : 1);  // active low
    }
}

Outcome Classify(esp_err_t err) {
    switch (err) {
        case ESP_OK: return Outcome::kOk;
        case ESP_FAIL: return Outcome::kNoFilesystem;  // the card is there, f_mount failed
        case ESP_ERR_TIMEOUT: return Outcome::kNoAnswer;
        default: return Outcome::kFailed;
    }
}

// A real sector read: card_init alone passes on a marginal data path that
// fails the first block read (FreeInk).
esp_err_t ReadSectorZero(sdmmc_card_t *c) {
    void *buf = heap_caps_malloc(kSectorSize, MALLOC_CAP_DMA);
    if (buf == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = sdmmc_read_sectors(c, buf, 0, 1);
    heap_caps_free(buf);
    return err;
}

Outcome Attempt(int i) {
    const boards::SdPins &pins = boards::kBoard.sd_pins;
    Power(false);
    vTaskDelay(pdMS_TO_TICKS(kPowerOffMs));
    Power(true);  // and left on: switching it off later breaks every read
    vTaskDelay(pdMS_TO_TICKS(kPowerOnMs));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = static_cast<int>(AttemptKhz(i));
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = static_cast<gpio_num_t>(pins.clk);
    slot.cmd = static_cast<gpio_num_t>(pins.cmd);
    slot.d0 = static_cast<gpio_num_t>(pins.d0);
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;  // never: the card holds the reader's books
    mount.max_files = 4;
    mount.allocation_unit_size = 0;

    sdmmc_card_t *c = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(kBasePath, &host, &slot, &mount, &c);
    if (err == ESP_OK) {
        err = ReadSectorZero(c);
        if (err != ESP_OK) {
            esp_vfs_fat_sdcard_unmount(kBasePath, c);
            ESP_LOGW(TAG, "attempt %d: sector 0: %s", i + 1, esp_err_to_name(err));
            return Outcome::kFailed;
        }
        card = c;
        return Outcome::kOk;
    }
    ESP_LOGW(TAG, "attempt %d at %u kHz: %s", i + 1, static_cast<unsigned>(AttemptKhz(i)),
             esp_err_to_name(err));
    return Classify(err);
}

}  // namespace

esp_err_t Init() {
    if (!boards::kBoard.has_sd_card || card != nullptr) {
        return card != nullptr ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    const int8_t pin = boards::kBoard.sd_pins.power_enable;
    if (pin != boards::kNoPin) {
        gpio_reset_pin(static_cast<gpio_num_t>(pin));
        gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
    }
    int no_answer = 0;
    for (int i = 0; i < kMaxAttempts; ++i) {
        status = Attempt(i);
        no_answer = status == Outcome::kNoAnswer ? no_answer + 1 : 0;
        if (!Retry(i, status, no_answer)) {
            break;
        }
    }
    if (status != Outcome::kOk) {
        ESP_LOGW(TAG, "%s", Name(status));
        return ESP_FAIL;
    }
    Info info = {};
    GetInfo(&info);
    char size[16];
    FormatBytes(info.bytes, size, sizeof size);
    ESP_LOGI(TAG, "%s %s at %s, %u kHz", info.name, size, kBasePath,
             static_cast<unsigned>(info.khz));
    return ESP_OK;
}

bool Mounted() { return card != nullptr; }

Outcome Status() { return status; }

esp_err_t GetInfo(Info *out) {
    if (card == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = Info{};
    memcpy(out->name, card->cid.name, sizeof card->cid.name);
    out->name[sizeof out->name - 1] = '\0';
    out->bytes = static_cast<uint64_t>(card->csd.capacity) * card->csd.sector_size;
    out->khz = static_cast<uint32_t>(card->real_freq_khz);
    out->high_capacity = (card->ocr & SD_OCR_SDHC_CAP) != 0;
    return esp_vfs_fat_info(kBasePath, &out->fs_total, &out->fs_free);
}

void PrintInfo() {
    if (!boards::kBoard.has_sd_card) {
        printf("sd card    none on this board\n");
        return;
    }
    Info info = {};
    if (card == nullptr) {
        printf("sd card    %s\n", Name(status));
        return;
    }
    const esp_err_t err = GetInfo(&info);
    char size[16];
    FormatBytes(info.bytes, size, sizeof size);
    printf("sd card    %s, %s%s, 1-bit, %u kHz\n", info.name, size,
           info.high_capacity ? " (SDHC/SDXC)" : "", static_cast<unsigned>(info.khz));
    printf("mounted    %s (FAT)\n", kBasePath);
    if (err != ESP_OK) {
        printf("fs         %s\n", esp_err_to_name(err));
        return;
    }
    char total[16];
    char free_text[16];
    FormatBytes(info.fs_total, total, sizeof total);
    FormatBytes(info.fs_free, free_text, sizeof free_text);
    printf("fs         %s free of %s\n", free_text, total);
}

}  // namespace sd_card
