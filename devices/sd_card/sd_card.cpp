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
uint32_t card_khz = 0;           // the clock the mount came up at
sdmmc_card_t *lent = nullptr;    // the card without FAT, while a USB host has it

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

// The board's slot: 1-bit, its pins, at `khz`.
void Configure(uint32_t khz, sdmmc_host_t *host, sdmmc_slot_config_t *slot) {
    const boards::SdPins &pins = boards::kBoard.sd_pins;
    *host = SDMMC_HOST_DEFAULT();
    host->max_freq_khz = static_cast<int>(khz);
    *slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot->width = 1;
    slot->clk = static_cast<gpio_num_t>(pins.clk);
    slot->cmd = static_cast<gpio_num_t>(pins.cmd);
    slot->d0 = static_cast<gpio_num_t>(pins.d0);
    slot->d1 = GPIO_NUM_NC;
    slot->d2 = GPIO_NUM_NC;
    slot->d3 = GPIO_NUM_NC;
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

// The bus lines low while the power is off: pulled up (as a driver leaves
// them) they feed the card and it never resets (mounted again after the card
// reader: no answer until a reboot).
void BusLow() {
    const boards::SdPins &pins = boards::kBoard.sd_pins;
    const int8_t lines[] = {pins.clk, pins.cmd, pins.d0};
    for (const int8_t p : lines) {
        const gpio_num_t g = static_cast<gpio_num_t>(p);
        gpio_reset_pin(g);
        gpio_pullup_dis(g);
        gpio_set_direction(g, GPIO_MODE_OUTPUT);
        gpio_set_level(g, 0);
    }
}

Outcome Attempt(int i) {
    BusLow();
    Power(false);
    vTaskDelay(pdMS_TO_TICKS(kPowerOffMs));
    Power(true);  // and left on: switching it off later breaks every read
    vTaskDelay(pdMS_TO_TICKS(kPowerOnMs));

    sdmmc_host_t host = {};
    sdmmc_slot_config_t slot = {};
    Configure(AttemptKhz(i), &host, &slot);

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
        card_khz = AttemptKhz(i);
        return Outcome::kOk;
    }
    ESP_LOGW(TAG, "attempt %d at %u kHz: %s", i + 1, static_cast<unsigned>(AttemptKhz(i)),
             esp_err_to_name(err));
    return Classify(err);
}

// The mount plan (sd_policy.h): attempts, each after a power cycle.
esp_err_t Mount() {
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

}  // namespace

esp_err_t Init() {
    if (!boards::kBoard.has_sd_card || card != nullptr) {
        return card != nullptr ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    return Mount();
}

esp_err_t Lend(sdmmc_card_t **out) {
    if (card == nullptr) {
        return lent != nullptr ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    // FAT off (the host and the card go with it), then the card alone again,
    // at the clock the mount had; the power stays on.
    esp_vfs_fat_sdcard_unmount(kBasePath, card);
    card = nullptr;
    sdmmc_host_t host = {};
    sdmmc_slot_config_t slot = {};
    Configure(card_khz, &host, &slot);
    auto *c = static_cast<sdmmc_card_t *>(heap_caps_calloc(1, sizeof(sdmmc_card_t),
                                                           MALLOC_CAP_INTERNAL));
    esp_err_t err = c != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = sdmmc_host_init();
    }
    if (err == ESP_OK) {
        err = sdmmc_host_init_slot(host.slot, &slot);
        if (err == ESP_OK) {
            err = sdmmc_card_init(&host, c);
        }
        if (err != ESP_OK) {
            sdmmc_host_deinit();
        }
    }
    if (err != ESP_OK) {
        heap_caps_free(c);
        ESP_LOGE(TAG, "the card for USB: %s", esp_err_to_name(err));
        Mount();  // FAT back if it can be
        return err;
    }
    lent = c;
    *out = c;
    ESP_LOGI(TAG, "FAT unmounted: the card is a USB drive");
    return ESP_OK;
}

esp_err_t TakeBack() {
    if (lent == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    sdmmc_host_deinit();
    heap_caps_free(lent);
    lent = nullptr;
    return Mount();
}

bool Lent() { return lent != nullptr; }

esp_err_t Remount() {
    if (card != nullptr || lent != nullptr || !boards::kBoard.has_sd_card) {
        return card != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    return Mount();
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
        printf("sd card    %s\n", lent != nullptr ? "a USB drive (usb reader): FAT unmounted"
                                                    : Name(status));
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
