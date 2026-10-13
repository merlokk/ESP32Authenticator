#include "web_paths.h"

namespace web {
namespace {

struct Extension {
    const char *suffix;
    const char *type;
};

constexpr Extension kServed[] = {
    {".html", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".png", "image/png"},
    {".ico", "image/x-icon"},
    {".svg", "image/svg+xml"},
    {".txt", "text/plain"},
};

size_t Length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

bool EndsWith(const char *name, size_t length, const char *suffix) {
    const size_t suffix_length = Length(suffix);
    if (suffix_length == 0 || suffix_length >= length) {
        return false;
    }
    const char *tail = name + (length - suffix_length);
    for (size_t i = 0; i < suffix_length; ++i) {
        if (tail[i] != suffix[i]) {
            return false;
        }
    }
    return true;
}

bool Allowed(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

}  // namespace

bool UriToName(const char *uri, char *out, size_t capacity) {
    if (uri == nullptr || out == nullptr || capacity == 0 || uri[0] != '/') {
        return false;
    }
    size_t length = 0;
    while (uri[1 + length] != '\0' && uri[1 + length] != '?' && uri[1 + length] != '#') {
        ++length;
    }
    const char *name = uri + 1;
    if (length == 0) {
        name = kIndexName;
        length = Length(kIndexName);
    }
    if (length > kMaxNameLength || length + 1 > capacity) {
        return false;
    }
    // '/' and '%' are not allowed characters: traversal cannot be spelled.
    for (size_t i = 0; i < length; ++i) {
        if (!Allowed(name[i])) {
            return false;
        }
    }
    if (name[0] == '.') {
        return false;
    }
    bool served = false;
    for (const Extension &extension : kServed) {
        if (EndsWith(name, length, extension.suffix)) {
            served = true;
            break;
        }
    }
    if (!served) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        out[i] = name[i];
    }
    out[length] = '\0';
    return true;
}

bool ConfirmsReboot(const char *uri) {
    if (uri == nullptr) {
        return false;
    }
    static constexpr char kWord[] = "confirm=reboot";
    constexpr size_t kWordLength = sizeof kWord - 1;
    const char *at = uri;
    while (*at != '\0' && *at != '?' && *at != '#') {
        ++at;
    }
    if (*at != '?') {
        return false;
    }
    ++at;
    // Whole parameters only: `confirm=reboots` is not a confirmation.
    for (;;) {
        size_t length = 0;
        while (at[length] != '\0' && at[length] != '&' && at[length] != '#') {
            ++length;
        }
        if (length == kWordLength) {
            size_t i = 0;
            while (i < kWordLength && at[i] == kWord[i]) {
                ++i;
            }
            if (i == kWordLength) {
                return true;
            }
        }
        if (at[length] != '&') {
            return false;
        }
        at += length + 1;
    }
}

const char *ContentType(const char *name) {
    if (name != nullptr) {
        const size_t length = Length(name);
        for (const Extension &extension : kServed) {
            if (EndsWith(name, length, extension.suffix)) {
                return extension.type;
            }
        }
    }
    return "application/octet-stream";
}

}  // namespace web
