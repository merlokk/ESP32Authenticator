#include "web_admin.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"

namespace web {
namespace {

void Say(BodyOutcome *out, BodyResult result, const char *detail) {
    out->result = result;
    std::snprintf(out->detail, sizeof out->detail, "%s", detail);
}

// Secrets must not linger in freed heap.
void WipeStrings(cJSON *item) {
    for (; item != nullptr; item = item->next) {
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            vault::Wipe(item->valuestring, std::strlen(item->valuestring));
        }
        WipeStrings(item->child);
    }
}

bool CopyString(const cJSON *item, char *field, size_t size, BodyOutcome *out) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        Say(out, BodyResult::kBadValue, item->string);
        return false;
    }
    const size_t length = std::strlen(item->valuestring);
    if (length >= size) {
        Say(out, BodyResult::kTooLong, item->string);
        return false;
    }
    std::memcpy(field, item->valuestring, length + 1);
    return true;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ReadField(const cJSON *item, AdminRequest *r, BodyOutcome *out) {
    const char *key = item->string;
    if (std::strcmp(key, "id") == 0) {
        const double v = cJSON_IsNumber(item) ? item->valuedouble : -1;
        if (v < 1 || v > 4294967295.0 || v != static_cast<double>(static_cast<uint32_t>(v))) {
            Say(out, BodyResult::kBadValue, key);
            return false;
        }
        r->has_id = true;
        r->id = static_cast<uint32_t>(v);
        return true;
    }
    if (std::strcmp(key, "position") == 0) {
        const double v = cJSON_IsNumber(item) ? item->valuedouble : -1;
        if (v < 0 || v > 65535 || v != static_cast<double>(static_cast<uint16_t>(v))) {
            Say(out, BodyResult::kBadValue, key);
            return false;
        }
        r->has_position = true;
        r->position = static_cast<uint16_t>(v);
        return true;
    }
    struct Text {
        const char *name;
        vault::Field field;
        char *into;
        size_t size;
    };
    const Text texts[] = {
        {"site", vault::Field::kSite, r->site, sizeof r->site},
        {"user", vault::Field::kUser, r->user, sizeof r->user},
        {"password", vault::Field::kPassword, r->password, sizeof r->password},
        {"otp", vault::Field::kOtp, r->otp, sizeof r->otp},
    };
    for (const Text &t : texts) {
        if (std::strcmp(key, t.name) == 0) {
            r->has[static_cast<size_t>(t.field)] = true;
            return CopyString(item, t.into, t.size, out);
        }
    }
    if (std::strcmp(key, "pin") == 0) {
        r->has_pin = true;
        return CopyString(item, r->pin, sizeof r->pin, out);
    }
    if (std::strcmp(key, "credential") == 0) {
        r->has_credential = true;
        if (!cJSON_IsString(item) || item->valuestring == nullptr ||
            !ParseCredentialId(item->valuestring, r->credential)) {
            Say(out, BodyResult::kBadValue, key);
            return false;
        }
        return true;
    }
    Say(out, BodyResult::kUnknownField, key);
    return false;
}

}  // namespace

VaultVerb VaultVerbFromUri(const char *uri) {
    static constexpr char kPrefix[] = "/api/vault/";
    constexpr size_t kPrefixLength = sizeof kPrefix - 1;
    if (uri == nullptr || std::strncmp(uri, kPrefix, kPrefixLength) != 0) {
        return VaultVerb::kNone;
    }
    const char *word = uri + kPrefixLength;
    const size_t length = std::strcspn(word, "?#");
    struct Verb {
        const char *word;
        VaultVerb verb;
    };
    static constexpr Verb kVerbs[] = {
        {"add", VaultVerb::kAdd},       {"set", VaultVerb::kSet},
        {"delete", VaultVerb::kDelete}, {"unlock", VaultVerb::kUnlock},
        {"lock", VaultVerb::kLock},     {"move", VaultVerb::kMove},
        {"sort", VaultVerb::kSort},
    };
    for (const Verb &v : kVerbs) {
        if (std::strlen(v.word) == length && std::strncmp(word, v.word, length) == 0) {
            return v.verb;
        }
    }
    return VaultVerb::kNone;
}

const char *VaultVerbName(VaultVerb verb) {
    switch (verb) {
        case VaultVerb::kAdd: return "add";
        case VaultVerb::kSet: return "set";
        case VaultVerb::kDelete: return "delete";
        case VaultVerb::kUnlock: return "unlock";
        case VaultVerb::kLock: return "lock";
        case VaultVerb::kMove: return "move";
        case VaultVerb::kSort: return "sort";
        case VaultVerb::kNone: break;
    }
    return "none";
}

BodyOutcome ParseAdminRequest(const char *body, size_t length, AdminRequest *out) {
    BodyOutcome outcome;
    vault::Wipe(out, sizeof *out);
    cJSON *root = cJSON_ParseWithLength(body, length);
    if (root == nullptr) {
        Say(&outcome, BodyResult::kNotJson, "");
        return outcome;
    }
    if (!cJSON_IsObject(root)) {
        Say(&outcome, BodyResult::kNotObject, "");
    } else {
        for (const cJSON *item = root->child; item != nullptr; item = item->next) {
            if (!ReadField(item, out, &outcome)) {
                break;
            }
        }
    }
    WipeStrings(root);
    cJSON_Delete(root);
    if (outcome.result != BodyResult::kOk) {
        vault::Wipe(out, sizeof *out);
    }
    return outcome;
}

const char *BodyResultText(BodyResult result) {
    switch (result) {
        case BodyResult::kOk: return "ok";
        case BodyResult::kNotJson: return "not JSON";
        case BodyResult::kNotObject: return "not a JSON object";
        case BodyResult::kUnknownField: return "unknown field";
        case BodyResult::kBadValue: return "bad value";
        case BodyResult::kTooLong: return "too long";
    }
    return "?";
}

const char *FieldValue(const AdminRequest &request, vault::Field field) {
    switch (field) {
        case vault::Field::kSite: return request.site;
        case vault::Field::kUser: return request.user;
        case vault::Field::kPassword: return request.password;
        case vault::Field::kOtp: return request.otp;
    }
    return "";
}

bool IsType(const char *content_type, const char *type) {
    if (content_type == nullptr || type == nullptr) {
        return false;
    }
    size_t i = 0;
    for (; type[i] != '\0'; ++i) {
        char c = content_type[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != type[i]) {
            return false;
        }
    }
    const char next = content_type[i];
    return next == '\0' || next == ';' || next == ' ';
}

bool IsJsonType(const char *content_type) { return IsType(content_type, "application/json"); }

bool IdFromQuery(const char *uri, uint32_t *id) {
    if (uri == nullptr || id == nullptr) {
        return false;
    }
    const char *at = std::strchr(uri, '?');
    if (at == nullptr) {
        return false;
    }
    ++at;
    for (;;) {
        const size_t length = std::strcspn(at, "&#");
        if (length > 3 && std::strncmp(at, "id=", 3) == 0) {
            uint64_t value = 0;
            size_t i = 3;
            for (; i < length && at[i] >= '0' && at[i] <= '9' && value <= 0xFFFFFFFFull; ++i) {
                value = value * 10 + static_cast<uint64_t>(at[i] - '0');
            }
            if (i == length && value >= 1 && value <= 0xFFFFFFFFull) {
                *id = static_cast<uint32_t>(value);
                return true;
            }
            return false;
        }
        if (at[length] != '&') {
            return false;
        }
        at += length + 1;
    }
}

bool ParseCredentialId(const char *hex, uint8_t out[kCredentialIdSize]) {
    if (hex == nullptr || std::strlen(hex) != 2 * kCredentialIdSize) {
        return false;
    }
    for (size_t i = 0; i < kCredentialIdSize; ++i) {
        const int high = HexDigit(hex[2 * i]);
        const int low = HexDigit(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>(high << 4 | low);
    }
    return true;
}

}  // namespace web
