#include "store_io.h"

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

#include "esp_log.h"
#include "nvs.h"

namespace storefile {

namespace {

constexpr const char *TAG = "storefile";
constexpr const char *kNvsVersion = "store_ver";
constexpr size_t kPathMax = 64;

bool NvsVersion(const File &file, uint32_t *out) {
    *out = 0;
    nvs_handle_t h;
    if (nvs_open(file.nvs_namespace, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    const bool found = nvs_get_u32(h, kNvsVersion, out) == ESP_OK;
    nvs_close(h);
    return found;
}

// Only when it grows (NVS wear: storage-encryption.md).
void KeepVersion(const File &file, uint32_t version) {
    uint32_t kept;
    if (NvsVersion(file, &kept) && kept >= version) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(file.nvs_namespace, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, kNvsVersion, version);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        // The file is newer than NVS: still accepted at the next boot.
        ESP_LOGE(TAG, "%s: version %u not kept: %s", file.path, static_cast<unsigned>(version),
                 esp_err_to_name(err));
    }
}

void TmpPath(const File &file, char *out) { snprintf(out, kPathMax, "%s.tmp", file.path); }

void RecoverTmp(const File &file) {
    char tmp[kPathMax];
    TmpPath(file, tmp);
    struct stat st;
    if (stat(tmp, &st) != 0) {
        return;
    }
    if (stat(file.path, &st) == 0) {
        ESP_LOGW(TAG, "%s: an unfinished save, dropped", tmp);
        remove(tmp);
    } else if (rename(tmp, file.path) == 0) {
        ESP_LOGW(TAG, "%s: a save cut before its rename, taken", tmp);
    } else {
        ESP_LOGE(TAG, "rename %s failed", tmp);
    }
}

Loaded Refuse(const File &file, const char *reason, const char **why) {
    ESP_LOGE(TAG, "%s not loaded: %s", file.path, reason);
    *why = reason;
    return Loaded::kRefused;
}

}  // namespace

uint32_t KeptVersion(const File &file) {
    uint32_t v;
    NvsVersion(file, &v);
    return v;
}

void Forget(const File &file) {
    char tmp[kPathMax];
    TmpPath(file, tmp);
    remove(tmp);
    remove(file.path);
    nvs_handle_t h;
    if (nvs_open(file.nvs_namespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, kNvsVersion);
        nvs_commit(h);
        nvs_close(h);
    }
}

size_t FileSize(const File &file) {
    struct stat st;
    return stat(file.path, &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
}

Loaded Load(const File &file, uint8_t *body, size_t capacity, Header *header, size_t *body_size,
            uint8_t raw[kHeaderSize], const char **why) {
    *body_size = 0;
    *why = nullptr;
    RecoverTmp(file);
    uint32_t kept = 0;
    const bool has_kept = NvsVersion(file, &kept);
    struct stat st;
    if (stat(file.path, &st) != 0) {
        if (errno != ENOENT) {
            return Refuse(file, "the store file cannot be read", why);
        }
        // Deleted (or the file system formatted) while a version is kept:
        // an empty start would let an old copy in next.
        return has_kept ? Refuse(file, "the store file is missing", why) : Loaded::kNew;
    }
    FILE *f = fopen(file.path, "rb");
    if (f == nullptr) {
        return Refuse(file, "the store file cannot be read", why);
    }
    const size_t file_size = static_cast<size_t>(st.st_size);
    uint8_t *head = raw;
    const size_t got = fread(head, 1, kHeaderSize, f);
    Header h;
    const Parse parse = Decode(head, got, file_size, file.kind, &h);
    const char *reason = nullptr;
    if (parse != Parse::kOk) {
        reason = "the store file is damaged or of an unknown format";
    } else if (Check(parse, h.version, has_kept, kept) == Verdict::kRollback) {
        ESP_LOGE(TAG, "%s: version %u, but %u was seen", file.path,
                 static_cast<unsigned>(h.version), static_cast<unsigned>(kept));
        reason = "the store file is older than one this device has used (rollback)";
    } else if (file_size - h.size > capacity) {
        reason = "the store file is larger than the store";
    } else if (fseek(f, h.size, SEEK_SET) != 0 ||
               fread(body, 1, file_size - h.size, f) != file_size - h.size) {
        reason = "the store file cannot be read";
    }
    fclose(f);
    if (reason != nullptr) {
        return Refuse(file, reason, why);
    }
    *header = h;
    *body_size = file_size - h.size;
    KeepVersion(file, h.version);  // above NVS: written before a power loss, or synced
    return Loaded::kOk;
}

uint32_t NextVersion(const File &file, uint32_t current) {
    return NextVersion(current, KeptVersion(file));
}

esp_err_t Save(const File &file, const Header &header, const uint8_t *body, size_t body_size) {
    Header h = header;
    h.kind = file.kind;
    uint8_t head[kHeaderSize];
    Encode(h, head);
    char tmp[kPathMax];
    TmpPath(file, tmp);
    FILE *f = fopen(tmp, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "cannot write %s", tmp);
        return ESP_FAIL;
    }
    const bool ok = fwrite(head, 1, sizeof head, f) == sizeof head &&
                    fwrite(body, 1, body_size, f) == body_size;
    if (fclose(f) != 0 || !ok) {
        ESP_LOGE(TAG, "write %s failed", tmp);
        remove(tmp);
        return ESP_FAIL;
    }
    remove(file.path);  // neither SPIFFS nor FAT renames over a file
    if (rename(tmp, file.path) != 0) {
        ESP_LOGE(TAG, "rename %s failed", tmp);
        return ESP_FAIL;
    }
    KeepVersion(file, h.version);  // after the file: a power loss between is a newer file
    return ESP_OK;
}

}  // namespace storefile
