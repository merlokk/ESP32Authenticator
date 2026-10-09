#include "spiffs_fs.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_spiffs.h"

namespace spiffs_fs {

namespace {

constexpr const char *TAG = "spiffs";
constexpr size_t kMaxOpenFiles = 5;

}  // namespace

esp_err_t Init() {
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = kBasePath,
        .partition_label = kPartitionLabel,
        .max_files = kMaxOpenFiles,
        .format_if_mount_failed = false,
    };
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount '%s' failed: %s", kPartitionLabel, esp_err_to_name(err));
        return err;
    }
    size_t total = 0;
    size_t used = 0;
    Info(&total, &used);
    ESP_LOGI(TAG, "mounted at %s, %u of %u bytes used", kBasePath, static_cast<unsigned>(used),
             static_cast<unsigned>(total));
    return ESP_OK;
}

bool Mounted() { return esp_spiffs_mounted(kPartitionLabel); }

esp_err_t Format() {
    // Mounted: IDF unmounts, formats and remounts. Not mounted (mount failed):
    // IDF formats and releases the partition, so it has to be mounted here.
    const bool was_mounted = Mounted();
    ESP_LOGW(TAG, "formatting '%s'", kPartitionLabel);
    esp_err_t err = esp_spiffs_format(kPartitionLabel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        return err;
    }
    return was_mounted ? ESP_OK : Init();
}

esp_err_t Info(size_t *total, size_t *used) {
    return esp_spiffs_info(kPartitionLabel, total, used);
}

bool FullPath(const char *name, char *out, size_t out_size) {
    const size_t base_len = strlen(kBasePath);
    int n = 0;
    if (strncmp(name, kBasePath, base_len) == 0 && name[base_len] == '/') {
        n = snprintf(out, out_size, "%s", name);
    } else {
        n = snprintf(out, out_size, "%s/%s", kBasePath, name[0] == '/' ? name + 1 : name);
    }
    return n > 0 && static_cast<size_t>(n) < out_size;
}

}  // namespace spiffs_fs
