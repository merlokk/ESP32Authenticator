#include "hardware.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "driver/temperature_sensor.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_flash_partitions.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/efuse_hal.h"
#include "nvs.h"

namespace hardware {

namespace {

const char *ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_EFUSE: return "eFuse error";
        case ESP_RST_PWR_GLITCH: return "power glitch";
        case ESP_RST_CPU_LOCKUP: return "CPU lockup";
        default: return "unknown";
    }
}

const char *KeyPurposeName(esp_efuse_purpose_t purpose) {
    switch (purpose) {
        case ESP_EFUSE_KEY_PURPOSE_USER: return "USER";
        case ESP_EFUSE_KEY_PURPOSE_RESERVED: return "RESERVED";
        case ESP_EFUSE_KEY_PURPOSE_XTS_AES_256_KEY_1: return "XTS_AES_256_KEY_1";
        case ESP_EFUSE_KEY_PURPOSE_XTS_AES_256_KEY_2: return "XTS_AES_256_KEY_2";
        case ESP_EFUSE_KEY_PURPOSE_XTS_AES_128_KEY: return "XTS_AES_128_KEY";
        case ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_ALL: return "HMAC_DOWN_ALL";
        case ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_JTAG: return "HMAC_DOWN_JTAG";
        case ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_DIGITAL_SIGNATURE: return "HMAC_DOWN_DS";
        case ESP_EFUSE_KEY_PURPOSE_HMAC_UP: return "HMAC_UP";
        case ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST0: return "SECURE_BOOT_DIGEST0";
        case ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST1: return "SECURE_BOOT_DIGEST1";
        case ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST2: return "SECURE_BOOT_DIGEST2";
        default: return "?";
    }
}

const char *OtaStateName(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "undefined";
    }
}

const char *FlashEncModeName(esp_flash_enc_mode_t mode) {
    switch (mode) {
        case ESP_FLASH_ENC_MODE_DISABLED: return "disabled";
        case ESP_FLASH_ENC_MODE_DEVELOPMENT: return "development";
        case ESP_FLASH_ENC_MODE_RELEASE: return "release";
        default: return "unknown";
    }
}

uint32_t ReadEfuse(const esp_efuse_desc_t *field[]) {
    uint32_t value = 0;
    esp_efuse_read_field_blob(field, &value, esp_efuse_get_field_size(field));
    return value;
}

// One eFuse bit: name, value, and what it means when burned.
void PrintFuse(const char *name, const esp_efuse_desc_t *field[], const char *meaning) {
    const uint32_t value = ReadEfuse(field);
    printf("%-34s %" PRIu32 "%s%s\n", name, value, value ? "  " : "", value ? meaning : "");
}

void PrintMac(const char *label, esp_mac_type_t type) {
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, type) == ESP_OK) {
        printf("%-11s%02x:%02x:%02x:%02x:%02x:%02x\n", label, mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5]);
    }
}

void PrintChip() {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    printf("[chip]\n");
    printf("model      %s rev v%" PRIu32 ".%" PRIu32 ", %d core(s), %d MHz\n", CONFIG_IDF_TARGET,
           efuse_hal_get_major_chip_version(), efuse_hal_get_minor_chip_version(), chip.cores,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    printf("features  %s%s%s%s%s\n", (chip.features & CHIP_FEATURE_WIFI_BGN) ? " wifi" : "",
           (chip.features & CHIP_FEATURE_BLE) ? " ble" : "",
           (chip.features & CHIP_FEATURE_BT) ? " bt" : "",
           (chip.features & CHIP_FEATURE_EMB_FLASH) ? " emb-flash" : "",
           (chip.features & CHIP_FEATURE_EMB_PSRAM) ? " emb-psram" : "");
    printf("package    %" PRIu32 "\n", ReadEfuse(ESP_EFUSE_PKG_VERSION));

    // ESP32-S3 has no dedicated chip ID: the factory base MAC serves as one, plus
    // the optional 128-bit unique ID in eFuse.
    PrintMac("base mac", ESP_MAC_BASE);
    PrintMac("wifi sta", ESP_MAC_WIFI_STA);
    PrintMac("wifi ap", ESP_MAC_WIFI_SOFTAP);
    PrintMac("bt", ESP_MAC_BT);

    uint8_t uid[16] = {};
    if (esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, sizeof uid * 8) == ESP_OK) {
        printf("unique id  ");
        for (uint8_t b : uid) {
            printf("%02x", b);
        }
        printf("\n");
    }
}

void PrintFlash() {
    printf("[flash]\n");
    uint32_t jedec = 0;
    if (esp_flash_read_id(nullptr, &jedec) == ESP_OK) {
        printf("jedec id   %06" PRIx32 " (mfr 0x%02" PRIx32 ")\n", jedec, jedec >> 16);
    }
    uint32_t physical = 0;
    uint32_t configured = 0;
    if (esp_flash_get_physical_size(nullptr, &physical) == ESP_OK &&
        esp_flash_get_size(nullptr, &configured) == ESP_OK) {
        printf("size       %" PRIu32 " KB physical, %" PRIu32 " KB configured\n", physical / 1024,
               configured / 1024);
    }
    uint64_t unique = 0;
    if (esp_flash_read_unique_chip_id(nullptr, &unique) == ESP_OK) {
        printf("unique id  %016" PRIx64 "\n", unique);
    }
    printf("efuse      vendor %" PRIu32 ", cap %" PRIu32 ", temp %" PRIu32 "\n",
           ReadEfuse(ESP_EFUSE_FLASH_VENDOR), ReadEfuse(ESP_EFUSE_FLASH_CAP),
           ReadEfuse(ESP_EFUSE_FLASH_TEMP));
}

void PrintMemory() {
    printf("[memory]\n");
    if (esp_psram_is_initialized()) {
        printf("psram      %u KB\n", static_cast<unsigned>(esp_psram_get_size() / 1024));
    } else {
        printf("psram      not initialized\n");
    }
    printf("efuse      psram vendor %" PRIu32 ", cap %" PRIu32 ", temp %" PRIu32 "\n",
           ReadEfuse(ESP_EFUSE_PSRAM_VENDOR), ReadEfuse(ESP_EFUSE_PSRAM_CAP),
           ReadEfuse(ESP_EFUSE_PSRAM_TEMP));
    printf("internal   %u free, %u lowest, %u largest block\n",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    printf("spiram     %u free, %u lowest, %u largest block\n",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

void PrintEfuse() {
    printf("[efuse]\n");
    printf("blk ver    v%" PRIu32 ".%" PRIu32 "\n", ReadEfuse(ESP_EFUSE_BLK_VERSION_MAJOR),
           ReadEfuse(ESP_EFUSE_BLK_VERSION_MINOR));
    printf("secure boot %s\n", esp_secure_boot_enabled() ? "ENABLED" : "disabled");
    printf("flash enc  %s (SPI_BOOT_CRYPT_CNT=%" PRIu32 ")\n",
           FlashEncModeName(esp_get_flash_encryption_mode()),
           ReadEfuse(ESP_EFUSE_SPI_BOOT_CRYPT_CNT));
    printf("enable_security_download %" PRIu32 "%s\n",
           ReadEfuse(ESP_EFUSE_ENABLE_SECURITY_DOWNLOAD),
           ReadEfuse(ESP_EFUSE_ENABLE_SECURITY_DOWNLOAD) ? "  ROM download limited to secure mode" : "");
    // Flashing and USB switches. A burned (1) bit cannot be cleared.
    PrintFuse("dis_download_mode", ESP_EFUSE_DIS_DOWNLOAD_MODE, "ROM download mode off entirely");
    PrintFuse("dis_usb_serial_jtag_download_mode", ESP_EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE,
              "no ROM flashing over USB Serial/JTAG; app console still works");
    PrintFuse("dis_usb_otg_download_mode", ESP_EFUSE_DIS_USB_OTG_DOWNLOAD_MODE,
              "no ROM flashing over USB-OTG");
    PrintFuse("dis_force_download", ESP_EFUSE_DIS_FORCE_DOWNLOAD,
              "app cannot reboot into download mode");
    PrintFuse("dis_download_manual_encrypt", ESP_EFUSE_DIS_DOWNLOAD_MANUAL_ENCRYPT,
              "no flash encryption in download mode");
    PrintFuse("dis_usb_serial_jtag", ESP_EFUSE_DIS_USB_SERIAL_JTAG,
              "USB Serial/JTAG controller off, cannot be re-enabled");
    PrintFuse("dis_usb_otg", ESP_EFUSE_DIS_USB_OTG, "USB-OTG controller off");
    PrintFuse("dis_usb_jtag", ESP_EFUSE_DIS_USB_JTAG, "JTAG over USB off");
    PrintFuse("dis_pad_jtag", ESP_EFUSE_DIS_PAD_JTAG, "JTAG on pins off");
    PrintFuse("dis_usb_serial_jtag_rom_print", ESP_EFUSE_DIS_USB_SERIAL_JTAG_ROM_PRINT,
              "no ROM boot log over USB Serial/JTAG");
    PrintFuse("dis_legacy_spi_boot", ESP_EFUSE_DIS_LEGACY_SPI_BOOT, "legacy SPI boot off");
    PrintFuse("dis_direct_boot", ESP_EFUSE_DIS_DIRECT_BOOT, "direct boot off");
    printf("soft_dis_jtag %" PRIu32 ", usb_phy_sel %" PRIu32 ", uart_print_control %" PRIu32 "\n",
           ReadEfuse(ESP_EFUSE_SOFT_DIS_JTAG), ReadEfuse(ESP_EFUSE_USB_PHY_SEL),
           ReadEfuse(ESP_EFUSE_UART_PRINT_CONTROL));
    printf("rd_dis     0x%02" PRIx32 ", wr_dis 0x%08" PRIx32 "\n", ReadEfuse(ESP_EFUSE_RD_DIS),
           ReadEfuse(ESP_EFUSE_WR_DIS));
    for (int i = 0; i < 6; ++i) {
        const auto block = static_cast<esp_efuse_block_t>(EFUSE_BLK_KEY0 + i);
        if (esp_efuse_key_block_unused(block)) {
            printf("key%d       unused\n", i);
            continue;
        }
        printf("key%d       %s%s%s\n", i, KeyPurposeName(esp_efuse_get_key_purpose(block)),
               esp_efuse_get_key_dis_read(block) ? ", read-protected" : "",
               esp_efuse_get_key_dis_write(block) ? ", write-protected" : "");
    }
}

void PrintNvs() {
    printf("[nvs]\n");
    nvs_stats_t stats = {};
    const esp_err_t err = nvs_get_stats(nullptr, &stats);
    if (err != ESP_OK) {
        printf("stats      %s\n", esp_err_to_name(err));
        return;
    }
    printf("entries    %u used, %u free, %u available, %u total\n",
           static_cast<unsigned>(stats.used_entries), static_cast<unsigned>(stats.free_entries),
           static_cast<unsigned>(stats.available_entries),
           static_cast<unsigned>(stats.total_entries));
    printf("namespaces %u\n", static_cast<unsigned>(stats.namespace_count));

    // Key count per namespace; key names and values are not printed.
    constexpr size_t kMaxNamespaces = 16;
    static char names[kMaxNamespaces][NVS_NS_NAME_MAX_SIZE];
    static unsigned counts[kMaxNamespaces];
    size_t found = 0;
    nvs_iterator_t it = nullptr;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (res == ESP_OK) {
        nvs_entry_info_t info = {};
        nvs_entry_info(it, &info);
        size_t i = 0;
        while (i < found && strcmp(names[i], info.namespace_name) != 0) {
            ++i;
        }
        if (i == found && found < kMaxNamespaces) {
            strlcpy(names[found], info.namespace_name, NVS_NS_NAME_MAX_SIZE);
            counts[found++] = 0;
        }
        if (i < found) {
            ++counts[i];
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    for (size_t i = 0; i < found; ++i) {
        printf("  %-15s %u key(s)\n", names[i], counts[i]);
    }
}

// Size of the app image actually written to a partition, 0 if none.
uint32_t ImageSize(const esp_partition_t *p) {
    const esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
    esp_image_metadata_t meta = {};
    return esp_image_get_metadata(&pos, &meta) == ESP_OK ? meta.image_len : 0;
}

void PrintRegion(const char *label, const char *type, uint32_t address, uint32_t size,
                 uint32_t used) {
    printf("  0x%06" PRIx32 "-0x%06" PRIx32 " %-10s %-13s %5" PRIu32 " KB", address,
           address + size - 1, label, type, size / 1024);
    if (used != 0) {
        printf(", image %" PRIu32 " KB (%" PRIu32 "%%)", used / 1024, used * 100 / size);
    }
}

const char *PartitionTypeName(const esp_partition_t *p) {
    static char text[24];
    if (p->type == ESP_PARTITION_TYPE_APP) {
        if (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            return "app/factory";
        }
        snprintf(text, sizeof text, "app/ota_%d", p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
        return text;
    }
    switch (p->subtype) {
        case ESP_PARTITION_SUBTYPE_DATA_OTA: return "data/ota";
        case ESP_PARTITION_SUBTYPE_DATA_PHY: return "data/phy";
        case ESP_PARTITION_SUBTYPE_DATA_NVS: return "data/nvs";
        case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "data/coredump";
        case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "data/nvs_keys";
        case ESP_PARTITION_SUBTYPE_DATA_FAT: return "data/fat";
        case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "data/spiffs";
        case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS: return "data/littlefs";
        default:
            snprintf(text, sizeof text, "%02x/%02x", p->type, p->subtype);
            return text;
    }
}

// Whole flash map: bootloader, partition table, partitions, and unused gaps.
void PrintFlashLayout() {
    printf("[flash layout]\n");
    uint32_t flash_size = 0;
    esp_flash_get_physical_size(nullptr, &flash_size);

    // On the X4 Pro this is the stock bootloader, not the one built here.
    PrintRegion("bootloader", "", CONFIG_BOOTLOADER_OFFSET_IN_FLASH,
                CONFIG_PARTITION_TABLE_OFFSET - CONFIG_BOOTLOADER_OFFSET_IN_FLASH, 0);
    esp_bootloader_desc_t boot_desc = {};
    if (esp_ota_get_bootloader_description(nullptr, &boot_desc) == ESP_OK) {
        printf(", v%" PRIu32 " idf %s, %s", boot_desc.version, boot_desc.idf_ver,
               boot_desc.date_time);
    }
    printf("\n");
    PrintRegion("ptable", "", CONFIG_PARTITION_TABLE_OFFSET, SPI_FLASH_SEC_SIZE, 0);
    printf("\n");

    uint32_t next = CONFIG_PARTITION_TABLE_OFFSET + SPI_FLASH_SEC_SIZE;
    uint32_t mapped = next;
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it != nullptr; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p->address > next) {
            PrintRegion("<free>", "", next, p->address - next, 0);
            printf("\n");
        }
        PrintRegion(p->label, PartitionTypeName(p), p->address, p->size,
                    p->type == ESP_PARTITION_TYPE_APP ? ImageSize(p) : 0);
        printf("\n");
        next = p->address + p->size;
        mapped += p->size;
    }
    esp_partition_iterator_release(it);
    if (flash_size > next) {
        PrintRegion("<free>", "", next, flash_size - next, 0);
        printf("\n");
    }
    printf("  total %" PRIu32 " KB, mapped %" PRIu32 " KB\n", flash_size / 1024, mapped / 1024);
}

// Raw otadata: two sectors, each with a sequence number. The valid entry with
// the higher seq selects the boot slot: (seq - 1) % number of OTA slots.
void PrintOtaData(const esp_partition_t *otadata) {
    for (int i = 0; i < 2; ++i) {
        esp_ota_select_entry_t entry = {};
        if (esp_partition_read(otadata, i * SPI_FLASH_SEC_SIZE, &entry, sizeof entry) != ESP_OK) {
            continue;
        }
        if (entry.ota_seq == UINT32_MAX) {
            printf("otadata[%d] empty\n", i);
            continue;
        }
        const bool crc_ok =
            esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t *>(&entry.ota_seq),
                             sizeof entry.ota_seq) == entry.crc;
        printf("otadata[%d] seq %" PRIu32 ", state %s, crc %s\n", i, entry.ota_seq,
               OtaStateName(static_cast<esp_ota_img_states_t>(entry.ota_state)),
               crc_ok ? "ok" : "BAD");
    }
}

void PrintOta() {
    printf("[ota]\n");
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    printf("slots      %u\n", esp_ota_get_app_partition_count());
    printf("running    %s\n", running != nullptr ? running->label : "-");
    printf("boot       %s\n", boot != nullptr ? boot->label : "-");
    printf("next       %s\n", next != nullptr ? next->label : "-");
    printf("invalid    %s\n", invalid != nullptr ? invalid->label : "-");
    printf("rollback   %s\n", esp_ota_check_rollback_is_possible() ? "possible" : "not possible");

    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it != nullptr; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        printf("  %-9s", p->label);
        esp_app_desc_t desc = {};
        if (esp_ota_get_partition_description(p, &desc) == ESP_OK) {
            printf(" %s %s (idf %s, %s %s)", desc.project_name, desc.version, desc.idf_ver,
                   desc.date, desc.time);
        } else {
            printf(" empty");
        }
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(p, &state) == ESP_OK) {
            printf(" [%s]", OtaStateName(state));
        }
        printf("\n");
    }
    esp_partition_iterator_release(it);

    const esp_partition_t *otadata =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (otadata != nullptr) {
        PrintOtaData(otadata);
    }
}

void PrintRuntime() {
    printf("[runtime]\n");
    float celsius = 0;
    if (CpuTemperature(&celsius) == ESP_OK) {
        printf("cpu temp   %.1f C\n", celsius);
    }
    const esp_reset_reason_t reason = esp_reset_reason();
    printf("reset      %s (%d)\n", ResetReasonName(reason), static_cast<int>(reason));
    const int64_t up = esp_timer_get_time() / 1000000;
    printf("uptime     %lldd %02lldh %02lldm %02llds\n", up / 86400, (up % 86400) / 3600,
           (up % 3600) / 60, up % 60);
}

}  // namespace

esp_err_t CpuTemperature(float *celsius) {
    // The driver logs its range on every install; keep the report clean.
    esp_log_level_set("temperature_sensor", ESP_LOG_WARN);
    temperature_sensor_handle_t tsens = nullptr;
    const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&config, &tsens);
    if (err != ESP_OK) {
        return err;
    }
    err = temperature_sensor_enable(tsens);
    if (err == ESP_OK) {
        err = temperature_sensor_get_celsius(tsens, celsius);
        temperature_sensor_disable(tsens);
    }
    temperature_sensor_uninstall(tsens);
    return err;
}

void PrintInfo() {
    PrintChip();
    PrintFlash();
    PrintMemory();
    PrintEfuse();
    PrintNvs();
    PrintFlashLayout();
    PrintOta();
    PrintRuntime();
}

}  // namespace hardware
