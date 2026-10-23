#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "cJSON.h"
#include "esp_log.h"
#include "zones.h"

namespace config {

// The built-in default config: used when there is no usable config.json.
const char kDefaultJson[] = R"json({
  "wifi": {
    "active": false,
    "mode": "client",
    "hostname": "esp32-auth",
    "rounds": 2,
    "apWindowSeconds": 120,
    "ap": {
      "ssid": "esp32-auth",
      "password": "esp32-auth",
      "channel": 6
    },
    "networks": []
  },
  "internet": {
    "check": true,
    "intervalSeconds": 60,
    "timeoutMs": 2000,
    "failures": 2,
    "targets": ["8.8.8.8", "1.1.1.1", "9.9.9.9"]
  },
  "ble": {
    "active": false,
    "name": ""
  },
  "web": {
    "mode": "auto",
    "write": true,
    "user": "",
    "password": ""
  },
  "time": {
    "zone": "UTC",
    "server": "pool.ntp.org",
    "syncHours": 6
  },
  "led": {
    "brightness": 15,
    "idleBrightness": 7
  },
  "frontlight": {
    "brightness": 20,
    "warmth": 50
  },
  "display": {
    "wallpaper": "grey"
  },
  "power": {
    "idleMinutes": 10,
    "keepPinHours": 0
  },
  "pin": {
    "lockMinutes": 10,
    "wipe": "",
    "complexity": true
  },
  "test": {
    "pin": "",
    "autoPresence": false
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

bool ValidWebPassword(const char *password) { return strlen(password) < kWebPasswordSize; }

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

// Optional integer member in [min, max].
bool GetUint(const cJSON *obj, const char *key, uint32_t min, uint32_t max, uint32_t *out,
             const char *where, char *error, size_t size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == nullptr) {
        return true;
    }
    const double value = cJSON_IsNumber(item) ? item->valuedouble : -1;
    if (value < min || value > max || value != static_cast<double>(static_cast<uint32_t>(value))) {
        char text[kErrorSize];
        snprintf(text, sizeof text, "%s.%s: an integer %u..%u", where, key,
                 static_cast<unsigned>(min), static_cast<unsigned>(max));
        SetError(error, size, text);
        return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

// Optional string member naming one of an enum's words.
template <typename Mode>
bool GetMode(const cJSON *obj, const char *key, bool (*parse)(const char *, Mode *), Mode *out,
             const char *words, const char *where, char *error, size_t size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == nullptr) {
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == nullptr || !parse(item->valuestring, out)) {
        char text[kErrorSize];
        snprintf(text, sizeof text, "%s.%s: one of %s", where, key, words);
        SetError(error, size, text);
        return false;
    }
    return true;
}

bool ParseAp(const cJSON *wifi, Wifi *out, char *error, size_t size) {
    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(wifi, "ap");
    if (ap == nullptr) {
        return true;
    }
    if (!cJSON_IsObject(ap)) {
        SetError(error, size, "wifi.ap: expected an object");
        return false;
    }
    uint32_t channel = out->ap_channel;
    if (!GetString(ap, "ssid", out->ap_ssid, sizeof out->ap_ssid, ValidSsid, "1..32 bytes",
                   "wifi.ap", error, size) ||
        !GetString(ap, "password", out->ap_password, sizeof out->ap_password, ValidApPassword,
                   "empty or 8..63 characters", "wifi.ap", error, size) ||
        !GetUint(ap, "channel", 1, 13, &channel, "wifi.ap", error, size)) {
        return false;
    }
    out->ap_channel = static_cast<uint8_t>(channel);
    return true;
}

bool ParseWifi(const cJSON *wifi, Wifi *out, char *error, size_t size) {
    uint32_t rounds = out->rounds;
    uint32_t window = out->ap_window_seconds;
    if (!GetBool(wifi, "active", &out->active, "wifi", error, size) ||
        !GetMode(wifi, "mode", ParseWifiMode, &out->mode, "client, ap, apsta", "wifi", error,
                 size) ||
        !GetString(wifi, "hostname", out->hostname, sizeof out->hostname, ValidHostname,
                   "1..32 of a-z A-Z 0-9 '-', not starting/ending with '-'", "wifi", error,
                   size) ||
        !GetUint(wifi, "rounds", 1, 10, &rounds, "wifi", error, size) ||
        !GetUint(wifi, "apWindowSeconds", 30, 3600, &window, "wifi", error, size) ||
        !ParseAp(wifi, out, error, size)) {
        return false;
    }
    out->rounds = static_cast<uint8_t>(rounds);
    out->ap_window_seconds = static_cast<uint16_t>(window);
    return true;
}

bool ParseWeb(const cJSON *web, Web *out, char *error, size_t size) {
    return GetMode(web, "mode", ParseWebMode, &out->mode, "off, on, auto", "web", error, size) &&
           GetBool(web, "write", &out->write, "web", error, size) &&
           GetString(web, "user", out->user, sizeof out->user, ValidWebUser,
                     "0..32 characters, no ':'", "web", error, size) &&
           GetString(web, "password", out->password, sizeof out->password, ValidWebPassword,
                     "0..64 characters", "web", error, size);
}

bool ParseTime(const cJSON *section, Time *out, char *error, size_t size) {
    uint32_t hours = out->sync_hours;
    if (!GetString(section, "zone", out->zone, sizeof out->zone, ValidZone,
                   "a zone name (date zones) or a POSIX TZ rule", "time", error, size) ||
        !GetString(section, "server", out->server, sizeof out->server, ValidHost,
                   "empty or a host name / IPv4 address", "time", error, size) ||
        !GetUint(section, "syncHours", 0, kMaxSyncHours, &hours, "time", error, size)) {
        return false;
    }
    out->sync_hours = static_cast<uint8_t>(hours);
    return true;
}

bool ParseDisplay(const cJSON *section, Display *out, char *error, size_t size) {
    return GetMode(section, "wallpaper", ParseWallpaperMode, &out->wallpaper, "grey, bw", "display",
                   error, size);
}

bool ParsePower(const cJSON *section, Power *out, char *error, size_t size) {
    uint32_t idle = out->idle_minutes;
    uint32_t keep = out->keep_pin_hours;
    if (!GetUint(section, "idleMinutes", 0, kMaxIdleMinutes, &idle, "power", error, size) ||
        !GetUint(section, "keepPinHours", 0, kMaxKeepPinHours, &keep, "power", error, size)) {
        return false;
    }
    out->idle_minutes = static_cast<uint16_t>(idle);
    out->keep_pin_hours = static_cast<uint8_t>(keep);
    return true;
}

bool ParseFrontlight(const cJSON *section, Frontlight *out, char *error, size_t size) {
    uint32_t brightness = out->brightness;
    uint32_t warmth = out->warmth;
    if (!GetUint(section, "brightness", 1, 100, &brightness, "frontlight", error, size) ||
        !GetUint(section, "warmth", 0, 100, &warmth, "frontlight", error, size)) {
        return false;
    }
    out->brightness = static_cast<uint8_t>(brightness);
    out->warmth = static_cast<uint8_t>(warmth);
    return true;
}

bool ParseLed(const cJSON *section, Led *out, char *error, size_t size) {
    uint32_t percent = out->percent;
    uint32_t idle = out->idle_percent;
    if (!GetUint(section, "brightness", 0, 100, &percent, "led", error, size) ||
        !GetUint(section, "idleBrightness", 0, 100, &idle, "led", error, size)) {
        return false;
    }
    out->percent = static_cast<uint8_t>(percent);
    out->idle_percent = static_cast<uint8_t>(idle);
    return true;
}

bool ParsePin(const cJSON *section, Pin *out, char *error, size_t size) {
    uint32_t minutes = out->lock_minutes;
    if (!GetUint(section, "lockMinutes", 1, kMaxLockMinutes, &minutes, "pin", error, size)) {
        return false;
    }
    out->lock_minutes = static_cast<uint16_t>(minutes);
    return GetString(section, "wipe", out->wipe, sizeof out->wipe, ValidWipe,
                     "empty or a hash from 'pin wipe set'", "pin", error, size) &&
           GetBool(section, "complexity", &out->complexity, "pin", error, size);
}

bool ParseTest(const cJSON *section, Test *out, char *error, size_t size) {
    return GetString(section, "pin", out->pin, sizeof out->pin, ValidTestPin,
                     "empty or 6 digits", "test", error, size) &&
           GetBool(section, "autoPresence", &out->auto_presence, "test", error, size);
}

bool AlwaysValid(const char *) { return true; }

// Optional `ip` block of one network; checked as a whole by CheckStaticIp.
bool ParseIp(const cJSON *network, StaticIp *out, const char *where, char *error, size_t size) {
    const cJSON *ip = cJSON_GetObjectItemCaseSensitive(network, "ip");
    if (ip == nullptr) {
        return true;
    }
    char text[kErrorSize];
    char ip_where[40];
    snprintf(ip_where, sizeof ip_where, "%s.ip", where);
    if (!cJSON_IsObject(ip)) {
        snprintf(text, sizeof text, "%s: expected an object", ip_where);
        SetError(error, size, text);
        return false;
    }
    if (!GetBool(ip, "static", &out->enabled, ip_where, error, size)) {
        return false;
    }
    struct Field {
        const char *key;
        char *out;
    };
    const Field fields[] = {{"address", out->address}, {"netmask", out->netmask},
                            {"gateway", out->gateway}, {"dns1", out->dns1},
                            {"dns2", out->dns2}};
    for (const Field &field : fields) {
        if (!GetString(ip, field.key, field.out, kIpTextSize, AlwaysValid, "an IPv4 address",
                       ip_where, error, size)) {
            return false;
        }
    }
    char reason[kErrorSize] = {};
    if (!CheckStaticIp(*out, reason, sizeof reason)) {
        char full[sizeof ip_where + kErrorSize];  // cut to the caller's size by SetError
        snprintf(full, sizeof full, "%s.%s", ip_where, reason);
        SetError(error, size, full);
        return false;
    }
    return true;
}

bool ParseInternet(const cJSON *internet, InternetCheck *out, char *error, size_t size) {
    uint32_t interval = out->interval_seconds;
    uint32_t timeout = out->timeout_ms;
    uint32_t failures = out->failures;
    if (!GetBool(internet, "check", &out->check, "internet", error, size) ||
        !GetUint(internet, "intervalSeconds", 5, 3600, &interval, "internet", error, size) ||
        !GetUint(internet, "timeoutMs", 100, 10000, &timeout, "internet", error, size) ||
        !GetUint(internet, "failures", 1, 10, &failures, "internet", error, size)) {
        return false;
    }
    out->interval_seconds = static_cast<uint16_t>(interval);
    out->timeout_ms = static_cast<uint16_t>(timeout);
    out->failures = static_cast<uint8_t>(failures);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(internet, "targets");
    if (list == nullptr) {
        return true;
    }
    char text[kErrorSize];
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > static_cast<int>(kMaxProbeTargets)) {
        snprintf(text, sizeof text, "internet.targets: an array of at most %u IPv4 addresses",
                 static_cast<unsigned>(kMaxProbeTargets));
        SetError(error, size, text);
        return false;
    }
    // `out` is ParseInto's staging copy: a failure discards it whole.
    InternetCheck &parsed = *out;
    parsed.target_count = 0;
    memset(parsed.targets, 0, sizeof parsed.targets);
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, list) {
        uint32_t ignored = 0;
        if (!cJSON_IsString(item) || item->valuestring == nullptr ||
            strlen(item->valuestring) >= kIpTextSize || !ParseIpv4(item->valuestring, &ignored)) {
            snprintf(text, sizeof text, "internet.targets[%u]: an IPv4 address (no names)",
                     static_cast<unsigned>(parsed.target_count));
            SetError(error, size, text);
            return false;
        }
        snprintf(parsed.targets[parsed.target_count++], kIpTextSize, "%s", item->valuestring);
    }
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
    Wifi &parsed = *out;  // ParseInto's staging copy, as above
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
                       "empty, 8..63 characters or 64 hex digits", where, error, size) ||
            !ParseIp(item, &n.ip, where, error, size)) {
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
    return true;
}

bool ParseSections(const cJSON *root, Data *staged, char *error, size_t size);

// Applies the members present in `root` over `out`. Changes `out` only on success.
bool ParseInto(const cJSON *root, Data *out, char *error, size_t size) {
    if (!cJSON_IsObject(root)) {
        SetError(error, size, "not a JSON object");
        return false;
    }
    // Staged on the heap: Data is over a kilobyte and this runs on small task
    // stacks (main, console, httpd).
    Data *staged = static_cast<Data *>(malloc(sizeof(Data)));
    if (staged == nullptr) {
        SetError(error, size, "out of memory");
        return false;
    }
    *staged = *out;
    Data &parsed = *staged;
    const bool ok = ParseSections(root, &parsed, error, size);
    if (ok) {
        *out = parsed;
    }
    free(staged);
    return ok;
}

bool ParseSections(const cJSON *root, Data *staged, char *error, size_t size) {
    Data &parsed = *staged;
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (wifi != nullptr) {
        if (!cJSON_IsObject(wifi)) {
            SetError(error, size, "wifi: expected an object");
            return false;
        }
        if (!ParseWifi(wifi, &parsed.wifi, error, size) ||
            !ParseNetworks(wifi, &parsed.wifi, error, size)) {
            return false;
        }
    }
    const cJSON *internet = cJSON_GetObjectItemCaseSensitive(root, "internet");
    if (internet != nullptr) {
        if (!cJSON_IsObject(internet)) {
            SetError(error, size, "internet: expected an object");
            return false;
        }
        if (!ParseInternet(internet, &parsed.internet, error, size)) {
            return false;
        }
    }
    const cJSON *web = cJSON_GetObjectItemCaseSensitive(root, "web");
    if (web != nullptr) {
        if (!cJSON_IsObject(web)) {
            SetError(error, size, "web: expected an object");
            return false;
        }
        if (!ParseWeb(web, &parsed.web, error, size)) {
            return false;
        }
    }
    const cJSON *ble = cJSON_GetObjectItemCaseSensitive(root, "ble");
    if (ble != nullptr) {
        if (!cJSON_IsObject(ble)) {
            SetError(error, size, "ble: expected an object");
            return false;
        }
        if (!GetBool(ble, "active", &parsed.ble.active, "ble", error, size) ||
            !GetString(ble, "name", parsed.ble.name, sizeof parsed.ble.name, ValidBleName,
                       "empty or 1..29 bytes of UTF-8, no control characters", "ble", error,
                       size)) {
            return false;
        }
    }
    const cJSON *time_json = cJSON_GetObjectItemCaseSensitive(root, "time");
    if (time_json != nullptr) {
        if (!cJSON_IsObject(time_json)) {
            SetError(error, size, "time: expected an object");
            return false;
        }
        if (!ParseTime(time_json, &parsed.time, error, size)) {
            return false;
        }
    }
    const cJSON *led = cJSON_GetObjectItemCaseSensitive(root, "led");
    if (led != nullptr) {
        if (!cJSON_IsObject(led)) {
            SetError(error, size, "led: expected an object");
            return false;
        }
        if (!ParseLed(led, &parsed.led, error, size)) {
            return false;
        }
    }
    const cJSON *light = cJSON_GetObjectItemCaseSensitive(root, "frontlight");
    if (light != nullptr) {
        if (!cJSON_IsObject(light)) {
            SetError(error, size, "frontlight: expected an object");
            return false;
        }
        if (!ParseFrontlight(light, &parsed.frontlight, error, size)) {
            return false;
        }
    }
    const cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (display != nullptr) {
        if (!cJSON_IsObject(display)) {
            SetError(error, size, "display: expected an object");
            return false;
        }
        if (!ParseDisplay(display, &parsed.display, error, size)) {
            return false;
        }
    }
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");
    if (power != nullptr) {
        if (!cJSON_IsObject(power)) {
            SetError(error, size, "power: expected an object");
            return false;
        }
        if (!ParsePower(power, &parsed.power, error, size)) {
            return false;
        }
    }
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(root, "pin");
    if (pin != nullptr) {
        if (!cJSON_IsObject(pin)) {
            SetError(error, size, "pin: expected an object");
            return false;
        }
        if (!ParsePin(pin, &parsed.pin, error, size)) {
            return false;
        }
    }
    const cJSON *test = cJSON_GetObjectItemCaseSensitive(root, "test");
    if (test != nullptr) {
        if (!cJSON_IsObject(test)) {
            SetError(error, size, "test: expected an object");
            return false;
        }
        if (!ParseTest(test, &parsed.test, error, size)) {
            return false;
        }
    }
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
    Data *staged = static_cast<Data *>(malloc(sizeof(Data)));  // not on the stack
    if (staged == nullptr) {
        snprintf(last_error, sizeof last_error, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    Data &parsed = *staged;
    FillDefaults(&parsed);
    char reason[kErrorSize] = {};
    const bool ok = Parse(file_buffer, length, &parsed, reason, sizeof reason);
    if (ok) {
        *out = parsed;
    } else {
        snprintf(last_error, sizeof last_error, "%s", reason);
    }
    free(staged);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
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
    cJSON_AddStringToObject(wifi, "mode", Name(d.wifi.mode));
    cJSON_AddStringToObject(wifi, "hostname", d.wifi.hostname);
    cJSON_AddNumberToObject(wifi, "rounds", d.wifi.rounds);
    cJSON_AddNumberToObject(wifi, "apWindowSeconds", d.wifi.ap_window_seconds);
    cJSON *ap = cJSON_AddObjectToObject(wifi, "ap");
    cJSON_AddStringToObject(ap, "ssid", d.wifi.ap_ssid);
    cJSON_AddStringToObject(ap, "password", d.wifi.ap_password);
    cJSON_AddNumberToObject(ap, "channel", d.wifi.ap_channel);
    cJSON *list = cJSON_AddArrayToObject(wifi, "networks");
    for (uint8_t i = 0; i < d.wifi.network_count && i < kMaxNetworks; ++i) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", d.wifi.networks[i].ssid);
        cJSON_AddStringToObject(n, "password", d.wifi.networks[i].password);
        // Only when something is set: no empty block on every entry.
        const StaticIp &ip = d.wifi.networks[i].ip;
        if (ip.enabled || ip.address[0] != '\0' || ip.netmask[0] != '\0' ||
            ip.gateway[0] != '\0' || ip.dns1[0] != '\0' || ip.dns2[0] != '\0') {
            cJSON *block = cJSON_AddObjectToObject(n, "ip");
            cJSON_AddBoolToObject(block, "static", ip.enabled);
            cJSON_AddStringToObject(block, "address", ip.address);
            cJSON_AddStringToObject(block, "netmask", ip.netmask);
            cJSON_AddStringToObject(block, "gateway", ip.gateway);
            cJSON_AddStringToObject(block, "dns1", ip.dns1);
            cJSON_AddStringToObject(block, "dns2", ip.dns2);
        }
        cJSON_AddItemToArray(list, n);
    }
    cJSON *internet = cJSON_AddObjectToObject(root, "internet");
    cJSON_AddBoolToObject(internet, "check", d.internet.check);
    cJSON_AddNumberToObject(internet, "intervalSeconds", d.internet.interval_seconds);
    cJSON_AddNumberToObject(internet, "timeoutMs", d.internet.timeout_ms);
    cJSON_AddNumberToObject(internet, "failures", d.internet.failures);
    cJSON *targets = cJSON_AddArrayToObject(internet, "targets");
    for (uint8_t i = 0; i < d.internet.target_count && i < kMaxProbeTargets; ++i) {
        cJSON_AddItemToArray(targets, cJSON_CreateString(d.internet.targets[i]));
    }
    cJSON *ble = cJSON_AddObjectToObject(root, "ble");
    cJSON_AddBoolToObject(ble, "active", d.ble.active);
    cJSON_AddStringToObject(ble, "name", d.ble.name);
    cJSON *web = cJSON_AddObjectToObject(root, "web");
    cJSON_AddStringToObject(web, "mode", Name(d.web.mode));
    cJSON_AddBoolToObject(web, "write", d.web.write);
    cJSON_AddStringToObject(web, "user", d.web.user);
    cJSON_AddStringToObject(web, "password", d.web.password);
    cJSON *time_json = cJSON_AddObjectToObject(root, "time");
    cJSON_AddStringToObject(time_json, "zone", d.time.zone);
    cJSON_AddStringToObject(time_json, "server", d.time.server);
    cJSON_AddNumberToObject(time_json, "syncHours", d.time.sync_hours);
    cJSON *led = cJSON_AddObjectToObject(root, "led");
    cJSON_AddNumberToObject(led, "brightness", d.led.percent);
    cJSON_AddNumberToObject(led, "idleBrightness", d.led.idle_percent);
    cJSON *light = cJSON_AddObjectToObject(root, "frontlight");
    cJSON_AddNumberToObject(light, "brightness", d.frontlight.brightness);
    cJSON_AddNumberToObject(light, "warmth", d.frontlight.warmth);
    cJSON *display = cJSON_AddObjectToObject(root, "display");
    cJSON_AddStringToObject(display, "wallpaper", Name(d.display.wallpaper));
    cJSON *power = cJSON_AddObjectToObject(root, "power");
    cJSON_AddNumberToObject(power, "idleMinutes", d.power.idle_minutes);
    cJSON_AddNumberToObject(power, "keepPinHours", d.power.keep_pin_hours);
    cJSON *pin = cJSON_AddObjectToObject(root, "pin");
    cJSON_AddNumberToObject(pin, "lockMinutes", d.pin.lock_minutes);
    cJSON_AddStringToObject(pin, "wipe", d.pin.wipe);
    cJSON_AddBoolToObject(pin, "complexity", d.pin.complexity);
    cJSON *test = cJSON_AddObjectToObject(root, "test");
    cJSON_AddStringToObject(test, "pin", d.test.pin);
    cJSON_AddBoolToObject(test, "autoPresence", d.test.auto_presence);

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

bool ValidApPassword(const char *password) {
    const size_t n = strlen(password);
    return n == 0 || (n >= 8 && n <= 63);
}

// Basic auth separates user and password with the first ':'.
bool ValidWebUser(const char *user) {
    return strlen(user) < kWebUserSize && strchr(user, ':') == nullptr;
}

bool ValidWipe(const char *wipe) {
    const size_t n = strlen(wipe);
    if (n == 0) {
        return true;
    }
    if (n != kWipeSize - 1 || wipe[32] != ':') {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (i != 32 && strchr("0123456789abcdefABCDEF", wipe[i]) == nullptr) {
            return false;
        }
    }
    return true;
}

bool ValidTestPin(const char *pin) {
    const size_t n = strlen(pin);
    return n == 0 || (n == kPinSize - 1 && strspn(pin, "0123456789") == n);
}

bool ValidZone(const char *zone) {
    return strlen(zone) < kZoneSize && tz::Resolve(zone) != nullptr;
}

// Labels of letters, digits and '-', joined by single dots.
bool ValidBleName(const char *name) {
    const size_t n = strlen(name);
    if (n >= kBleNameSize) {
        return false;
    }
    // Well-formed UTF-8 (no overlongs or surrogates checked: a name, not a
    // security boundary), no control characters.
    for (size_t i = 0; i < n;) {
        const auto c = static_cast<unsigned char>(name[i]);
        size_t more = 0;
        if (c < 0x20 || c == 0x7F) {
            return false;
        } else if (c < 0x80) {
            more = 0;
        } else if ((c & 0xE0) == 0xC0) {
            more = 1;
        } else if ((c & 0xF0) == 0xE0) {
            more = 2;
        } else if ((c & 0xF8) == 0xF0) {
            more = 3;
        } else {
            return false;
        }
        // A cut sequence meets the NUL, which is no continuation byte.
        for (size_t k = 1; k <= more; ++k) {
            if ((static_cast<unsigned char>(name[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += more + 1;
    }
    return true;
}

bool ValidHost(const char *host) {
    const size_t n = strlen(host);
    if (n == 0) {
        return true;
    }
    if (n >= kHostSize || host[0] == '.' || host[0] == '-' || host[n - 1] == '.' ||
        host[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char c = host[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!ok || (c == '.' && (host[i + 1] == '.' || host[i + 1] == '-' || host[i - 1] == '-'))) {
            return false;
        }
    }
    return true;
}

bool ParseIpv4(const char *text, uint32_t *out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    uint32_t value = 0;
    const char *p = text;
    for (int octet = 0; octet < 4; ++octet) {
        if (octet > 0) {
            if (*p != '.') {
                return false;
            }
            ++p;
        }
        if (*p < '0' || *p > '9') {
            return false;  // empty, "1..2", a sign or a space
        }
        if (*p == '0' && p[1] >= '0' && p[1] <= '9') {
            return false;  // leading zero
        }
        uint32_t part = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            part = part * 10 + static_cast<uint32_t>(*p - '0');
            ++p;
            if (++digits > 3 || part > 255) {
                return false;
            }
        }
        value |= part << (8 * octet);
    }
    if (*p != '\0') {
        return false;
    }
    *out = value;
    return true;
}

bool CheckStaticIp(const StaticIp &ip, char *error, size_t error_size) {
    struct Field {
        const char *name;
        const char *text;
        bool required;
    };
    const Field fields[] = {{"address", ip.address, true}, {"netmask", ip.netmask, true},
                            {"gateway", ip.gateway, true}, {"dns1", ip.dns1, false},
                            {"dns2", ip.dns2, false}};
    for (const Field &field : fields) {
        uint32_t ignored = 0;
        char text[kErrorSize];
        if (field.text[0] == '\0') {
            if (ip.enabled && field.required) {
                snprintf(text, sizeof text, "%s: required for a static address", field.name);
                SetError(error, error_size, text);
                return false;
            }
        } else if (!ParseIpv4(field.text, &ignored)) {
            snprintf(text, sizeof text, "%s: '%s' is not an IPv4 address", field.name, field.text);
            SetError(error, error_size, text);
            return false;
        }
    }
    return true;
}

const char *Name(WifiMode mode) {
    switch (mode) {
        case WifiMode::kClient: return "client";
        case WifiMode::kAp: return "ap";
        case WifiMode::kApSta: return "apsta";
    }
    return "client";
}

bool ParseWifiMode(const char *text, WifiMode *out) {
    for (WifiMode mode : {WifiMode::kClient, WifiMode::kAp, WifiMode::kApSta}) {
        if (strcmp(text, Name(mode)) == 0) {
            *out = mode;
            return true;
        }
    }
    return false;
}

const char *Name(WebMode mode) {
    switch (mode) {
        case WebMode::kOff: return "off";
        case WebMode::kOn: return "on";
        case WebMode::kAuto: return "auto";
    }
    return "auto";
}

bool ParseWebMode(const char *text, WebMode *out) {
    for (WebMode mode : {WebMode::kOff, WebMode::kOn, WebMode::kAuto}) {
        if (strcmp(text, Name(mode)) == 0) {
            *out = mode;
            return true;
        }
    }
    return false;
}

const char *Name(WallpaperMode mode) {
    return mode == WallpaperMode::kBw ? "bw" : "grey";
}

bool ParseWallpaperMode(const char *text, WallpaperMode *out) {
    for (WallpaperMode mode : {WallpaperMode::kGrey, WallpaperMode::kBw}) {
        if (strcmp(text, Name(mode)) == 0) {
            *out = mode;
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
