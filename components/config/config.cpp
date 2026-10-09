#include "config.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"

namespace config {

// The built-in default config: used when there is no usable config.json.
const char kDefaultJson[] = R"json({
  "wifi": {
    "active": false,
    "hostname": "esp32-auth",
    "networks": []
  },
  "ble": {
    "active": false
  }
})json";

namespace {

constexpr const char *TAG = "config";

Data data;
Source source = Source::kDefault;
char dir[kMaxDirSize] = {};
char path[kMaxDirSize + 16] = {};
char temp_path[kMaxDirSize + 16] = {};
char last_error[kErrorSize] = {};
char file_buffer[kMaxFileSize + 1];

void SetError(char *error, size_t size, const char *text) {
    if (error != nullptr && size > 0) {
        snprintf(error, size, "%s", text);
    }
}

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool ValidSsid(const char *ssid) {
    const size_t n = strlen(ssid);
    return n >= 1 && n < kSsidSize;
}

// Empty (open network), a WPA passphrase of 8..63 chars, or a 64-hex-digit PSK.
bool ValidPassword(const char *password) {
    const size_t n = strlen(password);
    if (n == 0 || (n >= 8 && n <= 63)) {
        return true;
    }
    if (n != 64) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (!IsHex(password[i])) {
            return false;
        }
    }
    return true;
}

// RFC 1123 label: letters, digits, '-', not at either end.
bool ValidHostname(const char *name) {
    const size_t n = strlen(name);
    if (n == 0 || n >= kHostnameSize || name[0] == '-' || name[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Optional boolean member: absent keeps `*out`.
bool GetBool(const cJSON *obj, const char *key, bool *out, const char *where, char *error,
             size_t size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == nullptr) {
        return true;
    }
    if (!cJSON_IsBool(item)) {
        char text[kErrorSize];
        snprintf(text, sizeof text, "%s.%s: expected true or false", where, key);
        SetError(error, size, text);
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

// Optional string member, copied if it fits and passes `valid`.
bool GetString(const cJSON *obj, const char *key, char *out, size_t out_size,
               bool (*valid)(const char *), const char *rule, const char *where, char *error,
               size_t size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == nullptr) {
        return true;
    }
    char text[kErrorSize];
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        snprintf(text, sizeof text, "%s.%s: expected a string", where, key);
        SetError(error, size, text);
        return false;
    }
    if (strlen(item->valuestring) >= out_size || !valid(item->valuestring)) {
        snprintf(text, sizeof text, "%s.%s: %s", where, key, rule);
        SetError(error, size, text);
        return false;
    }
    snprintf(out, out_size, "%s", item->valuestring);
    return true;
}

bool ParseNetworks(const cJSON *wifi, Wifi *out, char *error, size_t size) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(wifi, "networks");
    if (list == nullptr) {
        return true;
    }
    char text[kErrorSize];
    if (!cJSON_IsArray(list)) {
        SetError(error, size, "wifi.networks: expected an array");
        return false;
    }
    if (cJSON_GetArraySize(list) > static_cast<int>(kMaxNetworks)) {
        snprintf(text, sizeof text, "wifi.networks: at most %u networks",
                 static_cast<unsigned>(kMaxNetworks));
        SetError(error, size, text);
        return false;
    }
    Wifi parsed = *out;
    parsed.network_count = 0;
    memset(parsed.networks, 0, sizeof parsed.networks);
    int index = 0;
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, list) {
        char where[32];
        snprintf(where, sizeof where, "wifi.networks[%d]", index++);
        if (!cJSON_IsObject(item)) {
            snprintf(text, sizeof text, "%s: expected an object", where);
            SetError(error, size, text);
            return false;
        }
        Network &n = parsed.networks[parsed.network_count];
        if (cJSON_GetObjectItemCaseSensitive(item, "ssid") == nullptr) {
            snprintf(text, sizeof text, "%s.ssid: missing", where);
            SetError(error, size, text);
            return false;
        }
        if (!GetString(item, "ssid", n.ssid, sizeof n.ssid, ValidSsid, "1..32 bytes", where,
                       error, size) ||
            !GetString(item, "password", n.password, sizeof n.password, ValidPassword,
                       "empty, 8..63 characters or 64 hex digits", where, error, size)) {
            return false;
        }
        for (uint8_t i = 0; i < parsed.network_count; ++i) {
            if (strcmp(parsed.networks[i].ssid, n.ssid) == 0) {
                snprintf(text, sizeof text, "%s.ssid: duplicate '%s'", where, n.ssid);
                SetError(error, size, text);
                return false;
            }
        }
        ++parsed.network_count;
    }
    *out = parsed;
    return true;
}

// Applies the members present in `root` over `out`. Changes `out` only on success.
bool ParseInto(const cJSON *root, Data *out, char *error, size_t size) {
    if (!cJSON_IsObject(root)) {
        SetError(error, size, "not a JSON object");
        return false;
    }
    Data parsed = *out;
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (wifi != nullptr) {
        if (!cJSON_IsObject(wifi)) {
            SetError(error, size, "wifi: expected an object");
            return false;
        }
        if (!GetBool(wifi, "active", &parsed.wifi.active, "wifi", error, size) ||
            !GetString(wifi, "hostname", parsed.wifi.hostname, sizeof parsed.wifi.hostname,
                       ValidHostname, "1..32 of a-z A-Z 0-9 '-', not starting/ending with '-'",
                       "wifi", error, size) ||
            !ParseNetworks(wifi, &parsed.wifi, error, size)) {
            return false;
        }
    }
    const cJSON *ble = cJSON_GetObjectItemCaseSensitive(root, "ble");
    if (ble != nullptr) {
        if (!cJSON_IsObject(ble)) {
            SetError(error, size, "ble: expected an object");
            return false;
        }
        if (!GetBool(ble, "active", &parsed.ble.active, "ble", error, size)) {
            return false;
        }
    }
    *out = parsed;
    return true;
}

bool FileExists(const char *file) {
    FILE *f = fopen(file, "rb");
    if (f == nullptr) {
        return false;
    }
    fclose(f);
    return true;
}

// A write interrupted between remove and rename leaves only the temp file.
void RecoverInterruptedWrite() {
    if (!FileExists(temp_path)) {
        return;
    }
    if (!FileExists(path)) {
        ESP_LOGW(TAG, "finishing an interrupted write");
        rename(temp_path, path);
    } else {
        remove(temp_path);  // the write never got to replace the file
    }
}

// Reads and parses the file into `out`. Sets last_error on failure.
esp_err_t Load(Data *out) {
    last_error[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        snprintf(last_error, sizeof last_error, "%s not found", kFileName);
        return ESP_ERR_NOT_FOUND;
    }
    const size_t length = fread(file_buffer, 1, sizeof file_buffer, f);
    fclose(f);
    if (length > kMaxFileSize) {
        snprintf(last_error, sizeof last_error, "%s larger than %u bytes", kFileName,
                 static_cast<unsigned>(kMaxFileSize));
        return ESP_ERR_INVALID_SIZE;
    }
    Data parsed;
    FillDefaults(&parsed);
    char reason[kErrorSize] = {};
    if (!Parse(file_buffer, length, &parsed, reason, sizeof reason)) {
        snprintf(last_error, sizeof last_error, "%s", reason);
        return ESP_ERR_INVALID_ARG;
    }
    *out = parsed;
    return ESP_OK;
}

}  // namespace

void FillDefaults(Data *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->wifi.hostname, sizeof out->wifi.hostname, "esp32-auth");
    cJSON *root = cJSON_Parse(kDefaultJson);
    if (root == nullptr || !ParseInto(root, out, nullptr, 0)) {
        ESP_LOGE(TAG, "built-in default config does not parse");
    }
    cJSON_Delete(root);
}

bool Parse(const char *json, size_t length, Data *out, char *error, size_t error_size) {
    cJSON *root = cJSON_ParseWithLength(json, length);
    if (root == nullptr) {
        SetError(error, error_size, "not valid JSON");
        return false;
    }
    const bool ok = ParseInto(root, out, error, error_size);
    cJSON_Delete(root);
    return ok;
}

size_t Serialize(const Data &d, char *out, size_t size) {
    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "active", d.wifi.active);
    cJSON_AddStringToObject(wifi, "hostname", d.wifi.hostname);
    cJSON *list = cJSON_AddArrayToObject(wifi, "networks");
    for (uint8_t i = 0; i < d.wifi.network_count && i < kMaxNetworks; ++i) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", d.wifi.networks[i].ssid);
        cJSON_AddStringToObject(n, "password", d.wifi.networks[i].password);
        cJSON_AddItemToArray(list, n);
    }
    cJSON *ble = cJSON_AddObjectToObject(root, "ble");
    cJSON_AddBoolToObject(ble, "active", d.ble.active);

    // cJSON wants 5 spare bytes to be sure it fits.
    const bool ok = size > 5 && cJSON_PrintPreallocated(root, out, static_cast<int>(size - 5), 1);
    cJSON_Delete(root);
    return ok ? strlen(out) : 0;
}

bool SetNetwork(Wifi *wifi, const char *ssid, const char *password, char *error,
                size_t error_size) {
    if (!ValidSsid(ssid)) {
        SetError(error, error_size, "ssid: 1..32 bytes");
        return false;
    }
    if (!ValidPassword(password)) {
        SetError(error, error_size, "password: empty, 8..63 characters or 64 hex digits");
        return false;
    }
    for (uint8_t i = 0; i < wifi->network_count; ++i) {
        if (strcmp(wifi->networks[i].ssid, ssid) == 0) {
            snprintf(wifi->networks[i].password, kPasswordSize, "%s", password);
            return true;
        }
    }
    if (wifi->network_count >= kMaxNetworks) {
        SetError(error, error_size, "network list is full");
        return false;
    }
    Network &n = wifi->networks[wifi->network_count++];
    snprintf(n.ssid, sizeof n.ssid, "%s", ssid);
    snprintf(n.password, sizeof n.password, "%s", password);
    return true;
}

bool RemoveNetwork(Wifi *wifi, const char *ssid) {
    for (uint8_t i = 0; i < wifi->network_count; ++i) {
        if (strcmp(wifi->networks[i].ssid, ssid) == 0) {
            for (uint8_t j = i; j + 1 < wifi->network_count; ++j) {
                wifi->networks[j] = wifi->networks[j + 1];
            }
            --wifi->network_count;
            memset(&wifi->networks[wifi->network_count], 0, sizeof(Network));
            return true;
        }
    }
    return false;
}

esp_err_t Init(const char *directory) {
    snprintf(dir, sizeof dir, "%s", directory);
    snprintf(path, sizeof path, "%s/%s", dir, kFileName);
    snprintf(temp_path, sizeof temp_path, "%s/%s", dir, kTempName);
    FillDefaults(&data);
    source = Source::kDefault;
    RecoverInterruptedWrite();
    const esp_err_t err = Load(&data);
    if (err == ESP_OK) {
        source = Source::kFile;
        ESP_LOGI(TAG, "loaded %s", path);
    } else {
        ESP_LOGW(TAG, "%s, using built-in defaults", last_error);
    }
    return err;
}

esp_err_t Reload() {
    if (path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = Load(&data);
    if (err == ESP_OK) {
        source = Source::kFile;
    }
    return err;
}

esp_err_t Save() {
    if (path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t length = Serialize(data, file_buffer, sizeof file_buffer);
    if (length == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen(temp_path, "wb");
    if (f == nullptr) {
        return ESP_FAIL;
    }
    const bool written = fwrite(file_buffer, 1, length, f) == length;
    const bool closed = fclose(f) == 0;
    if (!written || !closed) {
        remove(temp_path);
        return ESP_FAIL;
    }
    // SPIFFS rename does not replace an existing file: remove, then rename.
    // A cut in between leaves only the temp file, which Init() completes.
    remove(path);
    if (rename(temp_path, path) != 0) {
        return ESP_FAIL;
    }
    source = Source::kFile;
    last_error[0] = '\0';
    return ESP_OK;
}

Data &Get() { return data; }

Source LoadedFrom() { return source; }

const char *Path() { return path; }

const char *LastError() { return last_error; }

}  // namespace config
