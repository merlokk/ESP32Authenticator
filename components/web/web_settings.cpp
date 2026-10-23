#include "web_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"

namespace web {
namespace {

void Say(WriteOutcome *out, WriteResult result, const char *detail) {
    out->result = result;
    std::snprintf(out->detail, sizeof out->detail, "%s", detail != nullptr ? detail : "");
}

bool CopyField(const cJSON *value, char *field, size_t size, WriteOutcome *out,
               const char *name) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        Say(out, WriteResult::kBadValue, name);
        return false;
    }
    if (std::strlen(value->valuestring) >= size) {
        Say(out, WriteResult::kTooLong, name);
        return false;
    }
    std::snprintf(field, size, "%s", value->valuestring);
    return true;
}

// Absent or null keeps `keep`; a string replaces it if `valid` takes it.
bool CopyPassword(const cJSON *object, char *field, size_t size, const char *keep,
                  bool (*valid)(const char *), WriteOutcome *out, const char *name) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "password");
    if (value == nullptr || cJSON_IsNull(value)) {
        std::snprintf(field, size, "%s", keep);
        return true;
    }
    if (!CopyField(value, field, size, out, name)) {
        return false;
    }
    if (!valid(field)) {
        Say(out, WriteResult::kBadValue, name);
        return false;
    }
    return true;
}

bool ReadMode(const cJSON *item, config::Wifi *wifi, WriteOutcome *out) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        Say(out, WriteResult::kBadValue, "wifi.mode");
        return false;
    }
    if (std::strcmp(item->valuestring, "off") == 0) {
        wifi->active = false;  // the mode stays for the next "on"
        return true;
    }
    config::WifiMode mode;
    if (!config::ParseWifiMode(item->valuestring, &mode)) {
        Say(out, WriteResult::kBadValue, item->valuestring);
        return false;
    }
    wifi->active = true;
    wifi->mode = mode;
    return true;
}

bool ReadAp(const cJSON *ap, config::Wifi *wifi, const config::Wifi &before, WriteOutcome *out) {
    if (!cJSON_IsObject(ap)) {
        Say(out, WriteResult::kBadValue, "wifi.ap");
        return false;
    }
    for (const cJSON *item = ap->child; item != nullptr; item = item->next) {
        const char *key = item->string != nullptr ? item->string : "";
        if (std::strcmp(key, "ssid") == 0) {
            if (!CopyField(item, wifi->ap_ssid, sizeof wifi->ap_ssid, out, "wifi.ap.ssid")) {
                return false;
            }
            if (!config::ValidSsid(wifi->ap_ssid)) {
                Say(out, WriteResult::kBadValue, "wifi.ap.ssid");
                return false;
            }
        } else if (std::strcmp(key, "channel") == 0) {
            const double channel = cJSON_IsNumber(item) ? item->valuedouble : 0;
            if (channel < 1 || channel > 13 || channel != static_cast<int>(channel)) {
                Say(out, WriteResult::kBadValue, "wifi.ap.channel");
                return false;
            }
            wifi->ap_channel = static_cast<uint8_t>(channel);
        } else if (std::strcmp(key, "password") != 0) {
            Say(out, WriteResult::kUnknownField, key);
            return false;
        }
    }
    return CopyPassword(ap, wifi->ap_password, sizeof wifi->ap_password, before.ap_password,
                        config::ValidApPassword, out, "wifi.ap.password");
}

// Absent or null keeps `keep` (the page has no address fields); an object
// replaces it whole, `"static": false` is DHCP.
bool ReadIp(const cJSON *ip, config::StaticIp *into, const config::StaticIp &keep,
            WriteOutcome *out) {
    if (ip == nullptr || cJSON_IsNull(ip)) {
        *into = keep;
        return true;
    }
    *into = {};
    if (!cJSON_IsObject(ip)) {
        Say(out, WriteResult::kBadValue, "wifi.networks[].ip");
        return false;
    }
    struct Field {
        const char *name;
        char *text;
    };
    const Field fields[] = {{"address", into->address}, {"netmask", into->netmask},
                            {"gateway", into->gateway}, {"dns1", into->dns1},
                            {"dns2", into->dns2}};
    for (const cJSON *item = ip->child; item != nullptr; item = item->next) {
        const char *key = item->string != nullptr ? item->string : "";
        if (std::strcmp(key, "static") == 0) {
            if (!cJSON_IsBool(item)) {
                Say(out, WriteResult::kBadValue, "static");
                return false;
            }
            into->enabled = cJSON_IsTrue(item);
            continue;
        }
        const Field *field = nullptr;
        for (const Field &candidate : fields) {
            if (std::strcmp(key, candidate.name) == 0) {
                field = &candidate;
            }
        }
        if (field == nullptr) {
            Say(out, WriteResult::kUnknownField, key);
            return false;
        }
        if (!cJSON_IsNull(item) && !CopyField(item, field->text, config::kIpTextSize, out, key)) {
            return false;
        }
    }
    char reason[config::kErrorSize] = {};
    if (!config::CheckStaticIp(*into, reason, sizeof reason)) {
        // The reason starts with the field name ("gateway: ...").
        char name[sizeof out->detail] = {};
        for (size_t i = 0; i + 1 < sizeof name && reason[i] != '\0' && reason[i] != ':'; ++i) {
            name[i] = reason[i];
        }
        Say(out, WriteResult::kBadValue, name);
        return false;
    }
    return true;
}

bool ReadNetworks(const cJSON *list, config::Wifi *wifi, const config::Wifi &before,
                  WriteOutcome *out) {
    if (!cJSON_IsArray(list)) {
        Say(out, WriteResult::kBadValue, "wifi.networks");
        return false;
    }
    if (cJSON_GetArraySize(list) > static_cast<int>(config::kMaxNetworks)) {
        Say(out, WriteResult::kTooMany, "wifi.networks");
        return false;
    }
    config::Network fresh[config::kMaxNetworks] = {};
    uint8_t written = 0;
    for (const cJSON *entry = list->child; entry != nullptr; entry = entry->next) {
        if (!cJSON_IsObject(entry)) {
            Say(out, WriteResult::kBadValue, "wifi.networks[]");
            return false;
        }
        for (const cJSON *item = entry->child; item != nullptr; item = item->next) {
            const char *key = item->string != nullptr ? item->string : "";
            if (std::strcmp(key, "ssid") != 0 && std::strcmp(key, "password") != 0 &&
                std::strcmp(key, "ip") != 0) {
                Say(out, WriteResult::kUnknownField, key);
                return false;
            }
        }
        config::Network &network = fresh[written];
        if (!CopyField(cJSON_GetObjectItemCaseSensitive(entry, "ssid"), network.ssid,
                       sizeof network.ssid, out, "wifi.networks[].ssid")) {
            return false;
        }
        if (!config::ValidSsid(network.ssid)) {
            Say(out, WriteResult::kBadValue, "wifi.networks[].ssid");
            return false;
        }
        const char *keep = "";
        config::StaticIp keep_ip = {};
        for (uint8_t i = 0; i < before.network_count; ++i) {
            if (std::strcmp(before.networks[i].ssid, network.ssid) == 0) {
                keep = before.networks[i].password;
                keep_ip = before.networks[i].ip;
            }
        }
        for (uint8_t i = 0; i < written; ++i) {
            if (std::strcmp(fresh[i].ssid, network.ssid) == 0) {
                Say(out, WriteResult::kBadValue, network.ssid);  // duplicate
                return false;
            }
        }
        if (!CopyPassword(entry, network.password, sizeof network.password, keep,
                          config::ValidPassword, out, "wifi.networks[].password") ||
            !ReadIp(cJSON_GetObjectItemCaseSensitive(entry, "ip"), &network.ip, keep_ip, out)) {
            return false;
        }
        ++written;
    }
    for (uint8_t i = 0; i < config::kMaxNetworks; ++i) {
        wifi->networks[i] = fresh[i];
    }
    wifi->network_count = written;
    return true;
}

// `before` is the live config (untouched until success), `into` the staging copy.
bool ReadWifi(const cJSON *wifi, const config::Wifi &before, config::Data *into,
              WriteOutcome *out) {
    if (!cJSON_IsObject(wifi)) {
        Say(out, WriteResult::kBadValue, "wifi");
        return false;
    }
    for (const cJSON *item = wifi->child; item != nullptr; item = item->next) {
        const char *key = item->string != nullptr ? item->string : "";
        bool ok = false;
        if (std::strcmp(key, "mode") == 0) {
            ok = ReadMode(item, &into->wifi, out);
        } else if (std::strcmp(key, "ap") == 0) {
            ok = ReadAp(item, &into->wifi, before, out);
        } else if (std::strcmp(key, "networks") == 0) {
            ok = ReadNetworks(item, &into->wifi, before, out);
        } else {
            Say(out, WriteResult::kUnknownField, key);
        }
        if (!ok) {
            return false;
        }
    }
    out->wifi_changed = true;
    return true;
}

// `ble`: {"name": "..."}, nothing else.
bool ReadBle(const cJSON *ble, config::Data *into, WriteOutcome *out) {
    if (!cJSON_IsObject(ble)) {
        Say(out, WriteResult::kBadValue, "ble");
        return false;
    }
    for (const cJSON *item = ble->child; item != nullptr; item = item->next) {
        const char *key = item->string != nullptr ? item->string : "";
        if (std::strcmp(key, "name") != 0) {
            char detail[sizeof out->detail];
            std::snprintf(detail, sizeof detail, "ble.%s", key);
            Say(out, WriteResult::kUnknownField, detail);
            return false;
        }
        if (!cJSON_IsString(item) || item->valuestring == nullptr ||
            item->valuestring[0] == '\0' || !config::ValidBleName(item->valuestring)) {
            Say(out, WriteResult::kBadValue, "ble.name");
            return false;
        }
        std::snprintf(into->ble.name, sizeof into->ble.name, "%s", item->valuestring);
        out->ble_changed = true;
    }
    return true;
}

// `display`: {"wallpaper": "grey" | "bw"}, nothing else.
bool ReadDisplay(const cJSON *display, config::Data *into, WriteOutcome *out) {
    if (!cJSON_IsObject(display)) {
        Say(out, WriteResult::kBadValue, "display");
        return false;
    }
    for (const cJSON *item = display->child; item != nullptr; item = item->next) {
        const char *key = item->string != nullptr ? item->string : "";
        if (std::strcmp(key, "wallpaper") != 0) {
            char detail[sizeof out->detail];
            std::snprintf(detail, sizeof detail, "display.%s", key);
            Say(out, WriteResult::kUnknownField, detail);
            return false;
        }
        if (!cJSON_IsString(item) || item->valuestring == nullptr ||
            !config::ParseWallpaperMode(item->valuestring, &into->display.wallpaper)) {
            Say(out, WriteResult::kBadValue, "display.wallpaper");
            return false;
        }
    }
    return true;
}

}  // namespace

WriteOutcome ApplySettings(const char *body, size_t length, config::Data *into) {
    WriteOutcome out;
    if (body == nullptr || into == nullptr) {
        Say(&out, WriteResult::kNotJson, "");
        return out;
    }
    if (length > kMaxSettingsBody) {
        Say(&out, WriteResult::kTooBig, "");
        return out;
    }
    cJSON *root = cJSON_ParseWithLength(body, length);
    if (root == nullptr) {
        Say(&out, WriteResult::kNotJson, "");
        return out;
    }
    if (!cJSON_IsObject(root)) {
        Say(&out, WriteResult::kNotObject, "");
        cJSON_Delete(root);
        return out;
    }
    // Staged: a refusal anywhere leaves the live config as it was.
    // Staged on the heap: over a kilobyte, and httpd's stack is 4 KB.
    config::Data *staging = static_cast<config::Data *>(malloc(sizeof(config::Data)));
    if (staging == nullptr) {
        cJSON_Delete(root);
        Say(&out, WriteResult::kTooBig, "");
        return out;
    }
    *staging = *into;
    config::Data &staged = *staging;
    bool ok = true;
    for (const cJSON *section = root->child; section != nullptr && ok; section = section->next) {
        const char *key = section->string != nullptr ? section->string : "";
        if (std::strcmp(key, "wifi") == 0) {
            ok = ReadWifi(section, into->wifi, &staged, &out);
        } else if (std::strcmp(key, "ble") == 0) {
            ok = ReadBle(section, &staged, &out);
        } else if (std::strcmp(key, "display") == 0) {
            ok = ReadDisplay(section, &staged, &out);
        } else {
            Say(&out, WriteResult::kUnknownField, key);
            ok = false;
        }
    }
    cJSON_Delete(root);
    if (ok) {
        *into = staged;
    }
    free(staging);
    if (!ok) {
        out.wifi_changed = false;
        out.ble_changed = false;
        return out;
    }
    out.result = WriteResult::kOk;
    return out;
}

const char *WriteResultText(WriteResult result) {
    switch (result) {
        case WriteResult::kOk: return "ok";
        case WriteResult::kTooBig: return "body too large";
        case WriteResult::kNotJson: return "not valid JSON";
        case WriteResult::kNotObject: return "not a settings object";
        case WriteResult::kUnknownField: return "no such setting";
        case WriteResult::kBadValue: return "invalid value";
        case WriteResult::kTooLong: return "value too long";
        case WriteResult::kTooMany: return "too many networks";
    }
    return "refused";
}

Action ActionFromUri(const char *uri) {
    if (uri == nullptr) {
        return Action::kNone;
    }
    static constexpr char kKey[] = "do=";
    constexpr size_t kKeyLength = sizeof kKey - 1;
    const char *at = uri;
    while (*at != '\0' && *at != '?' && *at != '#') {
        ++at;
    }
    if (*at != '?') {
        return Action::kNone;
    }
    ++at;
    struct Verb {
        const char *word;
        Action action;
    };
    static constexpr Verb kVerbs[] = {
        {"save", Action::kSave},
        {"reload", Action::kReload},
        {"retry", Action::kWifiRetry},
    };
    for (;;) {
        size_t length = 0;
        while (at[length] != '\0' && at[length] != '&' && at[length] != '#') {
            ++length;
        }
        if (length > kKeyLength && std::strncmp(at, kKey, kKeyLength) == 0) {
            const char *value = at + kKeyLength;
            const size_t value_length = length - kKeyLength;
            for (const Verb &verb : kVerbs) {
                if (std::strlen(verb.word) == value_length &&
                    std::strncmp(value, verb.word, value_length) == 0) {
                    return verb.action;
                }
            }
            return Action::kNone;
        }
        if (at[length] != '&') {
            return Action::kNone;
        }
        at += length + 1;
    }
}

const char *ActionName(Action action) {
    switch (action) {
        case Action::kNone: return "nothing";
        case Action::kSave: return "save";
        case Action::kReload: return "reload";
        case Action::kWifiRetry: return "retry";
    }
    return "nothing";
}

}  // namespace web
